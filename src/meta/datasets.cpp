#include "ofexcel/datasets.hpp"

#include "ofexcel/db_util.hpp"

#include <algorithm>

namespace ofexcel {

namespace {

duckdb::Value NullableString(const std::string &text) {
	return text.empty() ? duckdb::Value(duckdb::LogicalType::VARCHAR) : duckdb::Value(text);
}

void DropDatasetTable(duckdb::Connection &connection, const duckdb::Value &name) {
	if (!name.IsNull()) {
		Execute(connection, "DROP TABLE IF EXISTS ds." + QuoteIdentifier(name.ToString()));
	}
}

} // namespace

std::string QuoteIdentifier(const std::string &name) {
	std::string quoted = "\"";
	for (const auto c : name) {
		quoted += c;
		if (c == '"') {
			quoted += '"';
		}
	}
	return quoted + "\"";
}

std::string InstanceTableName(const std::string &query_id, const std::string &instance_key, int64_t version) {
	return query_id + "__" + instance_key.substr(0, 12) + "__v" + std::to_string(version);
}

std::string StageTableName(const std::string &run_id) {
	auto compact = run_id;
	compact.erase(std::remove(compact.begin(), compact.end(), '-'), compact.end());
	return "stage__" + compact;
}

duckdb::LogicalType StorageType(const ColumnType &type) {
	switch (type.kind) {
	case ColumnKind::INTEGER:
		return duckdb::LogicalType::BIGINT;
	case ColumnKind::DECIMAL:
		return duckdb::LogicalType::DECIMAL(static_cast<uint8_t>(type.precision), static_cast<uint8_t>(type.scale));
	case ColumnKind::NUMBER:
		return duckdb::LogicalType::DOUBLE;
	case ColumnKind::DATE:
		return duckdb::LogicalType::DATE;
	case ColumnKind::TIMESTAMP:
	case ColumnKind::TIMESTAMPTZ:
		// timestamptz is stored as a plain TIMESTAMP already moved to UTC.
		return duckdb::LogicalType::TIMESTAMP;
	case ColumnKind::BOOLEAN:
		return duckdb::LogicalType::BOOLEAN;
	case ColumnKind::ID:
	case ColumnKind::STRING:
	case ColumnKind::TEXT:
		return duckdb::LogicalType::VARCHAR;
	}
	return duckdb::LogicalType::VARCHAR;
}

std::vector<SchemaColumn> ReadQuerySchema(duckdb::Connection &connection, const std::string &definition_hash) {
	std::vector<SchemaColumn> columns;
	for (const auto &row : QueryRows(connection,
	                                 "SELECT column_name, contract_type, header FROM meta.query_schemas "
	                                 "WHERE definition_hash = $1 ORDER BY ordinal",
	                                 {duckdb::Value(definition_hash)})) {
		SchemaColumn column;
		column.name = row[0].ToString();
		std::string error;
		if (!ParseColumnType(row[1].ToString(), column.type, error)) {
			column.type = ColumnType();
		}
		column.header = row[2].IsNull() ? "" : row[2].ToString();
		columns.push_back(std::move(column));
	}
	return columns;
}

void WriteQuerySchema(duckdb::Connection &connection, const std::string &definition_hash,
                      const std::vector<SchemaColumn> &columns) {
	QueryRows(connection, "DELETE FROM meta.query_schemas WHERE definition_hash = $1",
	          {duckdb::Value(definition_hash)});
	for (size_t ordinal = 0; ordinal < columns.size(); ordinal++) {
		const auto &column = columns[ordinal];
		QueryRows(connection, "INSERT INTO meta.query_schemas VALUES ($1, $2, $3, $4, $5)",
		          {duckdb::Value(definition_hash), duckdb::Value::INTEGER(static_cast<int32_t>(ordinal)),
		           duckdb::Value(column.name), duckdb::Value(column.type.ToString()), NullableString(column.header)});
	}
}

namespace {

std::string LowerCase(std::string text) {
	for (auto &c : text) {
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	return text;
}

ColumnType TypeOfStorage(const std::string &data_type) {
	auto lower = LowerCase(data_type);
	if (lower == "bigint") {
		lower = "integer";
	} else if (lower == "double") {
		lower = "number";
	} else if (lower != "date" && lower != "timestamp" && lower != "boolean" && lower.rfind("decimal(", 0) != 0) {
		lower = "string";
	}
	ColumnType type;
	std::string error;
	if (!ParseColumnType(lower, type, error)) {
		type = ColumnType();
	}
	return type;
}

} // namespace

std::vector<SchemaColumn> StoredSchema(duckdb::Connection &connection, const std::string &table,
                                       const std::string &definition_hash) {
	const auto stored = QueryRows(connection,
	                              "SELECT column_name, data_type FROM duckdb_columns() WHERE database_name = "
	                              "current_database() AND schema_name = 'ds' AND table_name = $1 ORDER BY column_index",
	                              {duckdb::Value(table)});
	const auto known = ReadQuerySchema(connection, definition_hash);
	std::vector<SchemaColumn> columns;
	for (const auto &row : stored) {
		SchemaColumn column;
		column.name = row[0].ToString();
		const auto data_type = row[1].ToString();
		column.type = TypeOfStorage(data_type);
		for (const auto &schema : known) {
			if (LowerCase(schema.name) == LowerCase(column.name) && StorageType(schema.type).ToString() == data_type) {
				column.type = schema.type;
				column.header = schema.header;
				break;
			}
		}
		columns.push_back(std::move(column));
	}
	return columns;
}

void RecordMissing(duckdb::Connection &connection, const std::string &workbook_id,
                   const std::vector<MissingInstance> &missing) {
	if (workbook_id.empty() || missing.empty()) {
		return;
	}
	std::lock_guard<std::mutex> guard(MetaWriteLock());
	InTransaction(connection, [&]() {
		const auto now = duckdb::Value::TIMESTAMP(duckdb::Timestamp::GetCurrentTimestamp());
		for (const auto &entry : missing) {
			QueryRows(connection,
			          "INSERT INTO meta.missing (workbook_id, instance_key, query_id, definition_hash, "
			          "connection_name, params_json, first_seen, last_seen, requests) "
			          "VALUES ($1, $2, $3, $4, $5, $6, $7, $7, 1) "
			          "ON CONFLICT (workbook_id, instance_key) DO UPDATE SET last_seen = excluded.last_seen, "
			          "requests = requests + 1",
			          {duckdb::Value(workbook_id), duckdb::Value(entry.instance_key), duckdb::Value(entry.query_id),
			           duckdb::Value(entry.definition_hash), duckdb::Value(entry.connection_name),
			           duckdb::Value(entry.params_json), now});
		}
	});
}

std::mutex &MetaWriteLock() {
	static std::mutex lock;
	return lock;
}

DatasetCleanup CleanupDatasets(duckdb::Connection &connection, int64_t ttl_days) {
	std::lock_guard<std::mutex> guard(MetaWriteLock());
	return InTransaction(connection, [&]() {
		DatasetCleanup cleanup;
		const auto now = duckdb::Value::TIMESTAMP(duckdb::Timestamp::GetCurrentTimestamp());

		const auto interrupted = QueryRows(connection,
		                                   "UPDATE meta.runs SET status = 'failed', finished_at = $1, "
		                                   "error_code = 'E_INTERNAL', "
		                                   "error_message = 'ofexceld stopped during the run' "
		                                   "WHERE status IN ('queued', 'running')",
		                                   {now});
		cleanup.interrupted_runs = interrupted.empty() ? 0 : interrupted[0][0].GetValue<int64_t>();

		for (const auto &row : QueryRows(connection, "SELECT table_name FROM duckdb_tables() "
		                                             "WHERE database_name = current_database() AND schema_name = 'ds' "
		                                             "AND starts_with(table_name, 'stage__')")) {
			DropDatasetTable(connection, row[0]);
			cleanup.stage_tables++;
		}

		const auto expired = QueryRows(connection,
		                               "SELECT instance_key, current_table, prev_table FROM meta.dataset_instances "
		                               "WHERE last_access_at < $1::TIMESTAMP - to_days($2::INTEGER)",
		                               {now, duckdb::Value::BIGINT(ttl_days)});
		for (const auto &row : expired) {
			DropDatasetTable(connection, row[1]);
			DropDatasetTable(connection, row[2]);
			QueryRows(connection, "DELETE FROM meta.dataset_instances WHERE instance_key = $1", {row[0]});
			cleanup.expired_instances++;
		}
		return cleanup;
	});
}

} // namespace ofexcel
