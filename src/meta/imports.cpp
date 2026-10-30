#include "ofexcel/imports.hpp"

#include "duckdb/common/types/uuid.hpp"
#include "duckdb/main/appender.hpp"
#include "ofexcel/api_error.hpp"
#include "ofexcel/datasets.hpp"
#include "ofexcel/db_util.hpp"
#include "ofexcel/hashing.hpp"
#include "ofexcel/query_file.hpp"
#include "ofexcel/query_registry.hpp"
#include "ofexcel/utf8.hpp"

#include <algorithm>
#include <cctype>
#include <iostream>

namespace ofexcel {

namespace {

constexpr size_t MAX_SOURCE_BYTES = 1024;
constexpr size_t MAX_COLUMN_NAME_BYTES = 255;
//! Excel's columns per sheet.
constexpr size_t MAX_IMPORT_COLUMNS = 16384;
//! Digits every BIGINT holds.
constexpr size_t BIGINT_DIGITS = 18;

ApiError BadRequest(const std::string &message) {
	return ApiError("E_BAD_REQUEST", 422, message);
}

std::string Lower(std::string text) {
	std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::tolower(c); });
	return text;
}

//! Valid UTF-8, not empty, at most `max_bytes`, no control characters.
bool IsPlainText(const std::string &text, size_t max_bytes) {
	return !text.empty() && text.size() <= max_bytes && IsValidUtf8(text) &&
	       std::none_of(text.begin(), text.end(), [](unsigned char c) { return c < 0x20 || c == 0x7f; });
}

bool IsIntegerText(const std::string &text) {
	const size_t from = !text.empty() && text[0] == '-' ? 1 : 0;
	return text.size() > from &&
	       std::all_of(text.begin() + static_cast<std::ptrdiff_t>(from), text.end(),
	                   [](unsigned char c) { return std::isdigit(c); });
}

//! The parameter type whose §3.1 rules read a cell of `type`; the cells of
//! id, string and text are read here.
ParamType CellType(const ColumnType &type) {
	ParamType param;
	switch (type.kind) {
	case ColumnKind::INTEGER:
		param.kind = ParamKind::INTEGER;
		break;
	case ColumnKind::NUMBER:
		param.kind = ParamKind::NUMBER;
		break;
	case ColumnKind::DECIMAL:
		param.kind = ParamKind::DECIMAL;
		param.precision = type.precision;
		param.scale = type.scale;
		break;
	case ColumnKind::DATE:
		param.kind = ParamKind::DATE;
		break;
	case ColumnKind::TIMESTAMP:
		param.kind = ParamKind::TIMESTAMP;
		break;
	case ColumnKind::BOOLEAN:
		param.kind = ParamKind::BOOLEAN;
		break;
	default:
		param.kind = ParamKind::STRING;
		break;
	}
	return param;
}

//! One cell as the value to store, NULL for an empty cell (null or "": Excel
//! does not tell them apart). Returns false and sets `error` if the value does
//! not fit the column.
bool CellValue(const ColumnType &type, const ParamInput &cell, duckdb::Value &value, std::string &error) {
	const auto storage = StorageType(type);
	if (cell.kind == ParamInput::Kind::MISSING || cell.kind == ParamInput::Kind::NULL_VALUE ||
	    (cell.kind == ParamInput::Kind::STRING && cell.text.empty())) {
		value = duckdb::Value(storage);
		return true;
	}
	switch (type.kind) {
	case ColumnKind::STRING:
	case ColumnKind::TEXT:
		// A string column of a sheet may hold numbers and booleans too.
		if (cell.kind == ParamInput::Kind::STRING || cell.kind == ParamInput::Kind::NUMBER) {
			value = duckdb::Value(cell.text);
			return true;
		}
		if (cell.kind == ParamInput::Kind::BOOLEAN) {
			value = duckdb::Value(cell.boolean ? "true" : "false");
			return true;
		}
		error = "expected a string, not a list";
		return false;
	case ColumnKind::ID:
		// Kept as written: the leading zeros are why the column is an id (§12).
		if ((cell.kind == ParamInput::Kind::STRING || cell.kind == ParamInput::Kind::NUMBER) &&
		    IsIntegerText(cell.text)) {
			value = duckdb::Value(cell.text);
			return true;
		}
		error = "expected an id: digits, as a string";
		return false;
	default:
		break;
	}
	const auto normalized = NormalizeParam(CellType(type), nullptr, cell);
	if (!normalized.ok) {
		error = normalized.message;
		return false;
	}
	const auto &canonical = normalized.value.scalar;
	if (type.kind == ColumnKind::INTEGER && canonical.size() - (canonical[0] == '-' ? 1 : 0) > BIGINT_DIGITS) {
		error = "the integer " + canonical + " is too large for type integer; import the column as id or number";
		return false;
	}
	value = duckdb::Value(canonical).DefaultCastAs(storage);
	return true;
}

} // namespace

ImportStore &ImportStore::Get() {
	static ImportStore store;
	return store;
}

ImportStore::ImportStore() : clock([]() { return std::chrono::steady_clock::now(); }) {
}

void ImportStore::SetClock(Clock clock_p) {
	std::lock_guard<std::mutex> lock(mutex);
	clock = std::move(clock_p);
}

ImportStore::Pending &ImportStore::Find(const std::string &import_id) {
	const auto found = pending.find(import_id);
	if (found == pending.end()) {
		throw ApiError("E_NOT_FOUND", 404,
		               "No import " + import_id + ": it was committed, or it expired after 10 minutes without rows");
	}
	return found->second;
}

std::string ImportStore::Begin(duckdb::Connection &connection, const std::string &name, const std::string &workbook_id,
                               const std::string &source, const std::vector<ImportColumn> &columns) {
	if (!IsIdentifier(name)) {
		throw BadRequest("The import name must be lower case letters, digits and _, starting with a letter, at most "
		                 "63 characters: not \"" +
		                 name + "\"");
	}
	if (!IsUuidV4(workbook_id)) {
		throw BadRequest("The workbook id must be a lowercase UUID v4");
	}
	if (!IsPlainText(source, MAX_SOURCE_BYTES)) {
		throw BadRequest("source must be the sheet range, such as Sheet1!tblBank: not empty, at most 1024 bytes, "
		                 "no control characters");
	}
	if (columns.empty() || columns.size() > MAX_IMPORT_COLUMNS) {
		throw BadRequest("An import has 1 to " + std::to_string(MAX_IMPORT_COLUMNS) + " columns");
	}
	Pending import;
	std::string definition;
	for (const auto &column : columns) {
		if (!IsPlainText(column.name, MAX_COLUMN_NAME_BYTES)) {
			throw BadRequest("A column name must not be empty, at most 255 bytes, without control characters");
		}
		for (const auto &seen : import.column_names) {
			if (Lower(seen) == Lower(column.name)) {
				throw BadRequest("Two columns are named " + column.name + "; column names must differ");
			}
		}
		ColumnType type;
		std::string error;
		if (!ParseColumnType(column.type, type, error) || type.kind == ColumnKind::TIMESTAMPTZ) {
			throw BadRequest("Column " + column.name + ": the type must be one of id, integer, decimal(p,s), number, "
			                                           "string, text, date, timestamp, boolean, not \"" +
			                 column.type + "\"");
		}
		definition += (definition.empty() ? "" : ", ") + QuoteIdentifier(column.name) + " " +
		              StorageType(type).ToString();
		import.column_names.push_back(column.name);
		import.column_types.push_back(type);
	}
	import.name = name;
	import.workbook_id = workbook_id;
	import.source = source;

	const auto import_id = duckdb::UUID::ToString(duckdb::UUID::GenerateRandomUUID());
	import.stage = StageTableName(import_id);
	std::lock_guard<std::mutex> lock(mutex);
	Execute(connection, "CREATE TABLE ds." + QuoteIdentifier(import.stage) + " (" + definition + ")");
	import.last_used = clock();
	pending[import_id] = std::move(import);
	return import_id;
}

void ImportStore::AddRows(duckdb::Connection &connection, const std::string &import_id,
                          const std::vector<std::vector<ParamInput>> &rows) {
	std::lock_guard<std::mutex> lock(mutex);
	auto &import = Find(import_id);
	if (import.rows + static_cast<int64_t>(rows.size()) > MAX_IMPORT_ROWS) {
		throw BadRequest("An import holds at most " + std::to_string(MAX_IMPORT_ROWS) + " rows");
	}
	// Every value first, so that a refused batch leaves nothing behind.
	std::vector<std::vector<duckdb::Value>> values;
	values.reserve(rows.size());
	for (size_t r = 0; r < rows.size(); r++) {
		const auto row_number = std::to_string(import.rows + static_cast<int64_t>(r) + 1);
		if (rows[r].size() != import.column_types.size()) {
			throw BadRequest("Row " + row_number + " has " + std::to_string(rows[r].size()) +
			                 " values; the import has " + std::to_string(import.column_types.size()) + " columns");
		}
		std::vector<duckdb::Value> row(rows[r].size());
		for (size_t c = 0; c < rows[r].size(); c++) {
			std::string error;
			if (!CellValue(import.column_types[c], rows[r][c], row[c], error)) {
				throw BadRequest("Row " + row_number + ", column " + import.column_names[c] + " (" +
				                 import.column_types[c].ToString() + "): " + error);
			}
		}
		values.push_back(std::move(row));
	}
	duckdb::Appender appender(connection, duckdb::Identifier(DATASET_SCHEMA), duckdb::Identifier(import.stage));
	for (auto &row : values) {
		appender.BeginRow();
		for (auto &value : row) {
			appender.Append(std::move(value));
		}
		appender.EndRow();
	}
	appender.Close();
	import.rows += static_cast<int64_t>(rows.size());
	import.last_used = clock();
}

CommittedImport ImportStore::Commit(duckdb::Connection &connection, const std::string &import_id) {
	std::lock_guard<std::mutex> lock(mutex);
	auto &import = Find(import_id);
	auto schema = CanonicalJson::Array();
	for (size_t i = 0; i < import.column_names.size(); i++) {
		schema.Add(CanonicalJson::Object()
		               .Set("name", CanonicalJson::String(import.column_names[i]))
		               .Set("type", CanonicalJson::String(import.column_types[i].ToString())));
	}
	const auto table = "xl_import." + QuoteIdentifier(import.name);
	{
		std::lock_guard<std::mutex> meta(MetaWriteLock());
		InTransaction(connection, [&]() {
			// Readers see the old table or the new one, never neither.
			Execute(connection, "DROP TABLE IF EXISTS " + table);
			Execute(connection, "CREATE TABLE " + table + " AS SELECT * FROM ds." + QuoteIdentifier(import.stage) +
			                        " ORDER BY rowid");
			Execute(connection, "DROP TABLE ds." + QuoteIdentifier(import.stage));
			Execute(connection, "UPDATE meta.state SET value = CAST(CAST(value AS BIGINT) + 1 AS VARCHAR) "
			                    "WHERE key = 'data_version'");
			QueryRows(connection,
			          "INSERT INTO meta.imports (name, workbook_id, source, schema_json, row_count, imported_at, "
			          "data_version) SELECT $1, $2, $3, $4, $5, $6, CAST(value AS BIGINT) FROM meta.state "
			          "WHERE key = 'data_version' "
			          "ON CONFLICT (name) DO UPDATE SET workbook_id = excluded.workbook_id, source = excluded.source, "
			          "schema_json = excluded.schema_json, row_count = excluded.row_count, "
			          "imported_at = excluded.imported_at, data_version = excluded.data_version",
			          {duckdb::Value(import.name), duckdb::Value(import.workbook_id), duckdb::Value(import.source),
			           duckdb::Value(schema.Serialize()), duckdb::Value::BIGINT(import.rows),
			           duckdb::Value::TIMESTAMP(duckdb::Timestamp::GetCurrentTimestamp())});
		});
	}
	CommittedImport committed {import.name, import.rows};
	pending.erase(import_id);
	return committed;
}

void ImportStore::DropExpired(duckdb::Connection &connection) {
	std::vector<std::string> stages;
	{
		std::lock_guard<std::mutex> lock(mutex);
		const auto now = clock();
		for (auto it = pending.begin(); it != pending.end();) {
			if (now - it->second.last_used >= IMPORT_EXPIRY) {
				stages.push_back(it->second.stage);
				it = pending.erase(it);
			} else {
				++it;
			}
		}
	}
	for (const auto &stage : stages) {
		try {
			Execute(connection, "DROP TABLE IF EXISTS ds." + QuoteIdentifier(stage));
		} catch (const std::exception &error) {
			std::cerr << "ofexceld: table ds." << stage << " not dropped: " << duckdb::ErrorData(error).Message()
			          << std::endl;
		}
	}
}

} // namespace ofexcel
