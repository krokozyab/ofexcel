// Preview (CONTRACTS.md §8.3): the query read as a run reads it -- through
// ofquack with a page of 200 rows (§5.1), or in DuckDB over the dependencies'
// current data (§5.3) -- but only as far as limit + 1 rows, held in memory,
// typed by §3.3 or §3.4 over those rows, and never stored.

#include "ofexcel/preview.hpp"

#include "duckdb/main/query_result_stream.hpp"
#include "ofexcel/api_error.hpp"
#include "ofexcel/connections.hpp"
#include "ofexcel/datasets.hpp"
#include "ofexcel/fusion_errors.hpp"
#include "ofexcel/hashing.hpp"
#include "ofexcel/local_types.hpp"
#include "ofexcel/oracle_render.hpp"
#include "ofexcel/query_file.hpp"
#include "ofexcel/result_types.hpp"
#include "run_work.hpp"

#include <chrono>
#include <functional>
#include <thread>

namespace ofexcel {

namespace {

//! What the query returned: at most `limit` rows.
struct ReadRows {
	std::vector<std::string> names;
	std::vector<duckdb::LogicalType> types;
	std::vector<std::vector<duckdb::Value>> rows;
	bool truncated = false;
	duckdb::ErrorData error;
};

//! The watchdog of a run, for a Preview: it interrupts the reader once
//! ofexcel_run_timeout has passed.
class Watched {
public:
	explicit Watched(int64_t timeout_seconds_p)
	    : timeout_seconds(timeout_seconds_p), watchdog([this]() { Watch(active, timeout_seconds); }) {
	}
	~Watched() {
		{
			std::lock_guard<std::mutex> lock(active.mutex);
			active.done = true;
		}
		active.changed.notify_all();
		watchdog.join();
	}

	//! Reads at most `limit` rows, and one more to know whether there are
	//! more (§5.4), from what `submit` starts on `reader`.
	ReadRows Read(duckdb::Connection &reader, int64_t limit,
	              const std::function<duckdb::unique_ptr<duckdb::QueryResult>()> &submit) {
		SetReader(&reader);
		struct ClearReader {
			Watched &self;
			~ClearReader() {
				self.SetReader(nullptr);
			}
		} clear_reader {*this};

		ReadRows read;
		auto handle = submit();
		if (handle->HasError()) {
			read.error = handle->GetErrorObject();
			return read;
		}
		duckdb::QueryResultStream<> stream(std::move(handle));
		for (const auto &name : stream.GetNames()) {
			read.names.push_back(name.GetIdentifierName());
		}
		for (const auto &type : stream.GetTypes()) {
			read.types.push_back(type);
		}
		while (!read.truncated) {
			duckdb::unique_ptr<duckdb::DataChunk> chunk;
			try {
				chunk = stream.Fetch();
			} catch (const std::exception &exception) {
				read.error = duckdb::ErrorData(exception);
				break;
			}
			if (!chunk) {
				break;
			}
			for (duckdb::idx_t row = 0; row < chunk->size(); row++) {
				if (static_cast<int64_t>(read.rows.size()) == limit) {
					read.truncated = true;
					break;
				}
				std::vector<duckdb::Value> values;
				for (duckdb::idx_t column = 0; column < chunk->ColumnCount(); column++) {
					values.push_back(chunk->GetValue(column, row));
				}
				read.rows.push_back(std::move(values));
			}
		}
		if (read.truncated) {
			// The rest is not needed: stop the query between chunks, or in one.
			reader.Interrupt();
			stream.Close();
		} else if (!read.error.HasError() && stream.HasError()) {
			read.error = stream.GetErrorObject();
		}
		return read;
	}

	//! E_TIMEOUT if the watchdog stopped the read, whatever error it caused.
	void ThrowIfTimedOut() const {
		if (active.timed_out) {
			throw ApiError("E_TIMEOUT", 504,
			               "The preview was stopped: it took longer than " + std::to_string(timeout_seconds) +
			                   " s (ofexcel_run_timeout)");
		}
	}

private:
	void SetReader(duckdb::Connection *connection) {
		std::lock_guard<std::mutex> lock(active.mutex);
		active.reader = connection;
	}

	int64_t timeout_seconds;
	RunEngine::Active active;
	std::thread watchdog;
};

const QueryColumn *Declared(const std::vector<QueryColumn> &declared, const std::string &name) {
	for (const auto &column : declared) {
		if (Lower(column.name) == Lower(name)) {
			return &column;
		}
	}
	return nullptr;
}

PreviewColumn Column(const std::string &name, const ColumnType &type, const QueryColumn *declared) {
	return {name, declared && !declared->header.empty() ? declared->header : name, type};
}

void PreviewOracle(duckdb::DatabaseInstance &db, duckdb::Connection &connection, const QueryHeader &header,
                   const PreviewRequest &request, Watched &watched, PreviewResult &result) {
	const auto connection_name = request.connection.empty() ? header.connection : request.connection;
	if (connection_name.empty()) {
		throw ApiError("E_BAD_REQUEST", 422,
		               "No connection given: pass connection or declare @connection in query " + header.id);
	}
	if (!FusionSecretExists(connection, connection_name)) {
		throw ApiError("E_NOT_FOUND", 404, "No connection " + connection_name);
	}
	const auto rendered = RenderOracle(header.body, header.params, request.params);
	if (!rendered.ok) {
		throw ApiError(rendered.code, 422, rendered.message);
	}
	result.oracle = true;
	result.rendered_sql = rendered.rendered;
	for (const auto &message : rendered.warnings) {
		result.warnings.push_back({WARNING_PARAM_UNUSED, message});
	}

	duckdb::Connection reader(db);
	auto prepared = reader.Prepare(ORACLE_QUERY_SQL);
	if (prepared->HasError()) {
		throw FusionApiError(prepared->GetErrorObject());
	}
	duckdb::vector<duckdb::Value> values {duckdb::Value(rendered.rendered), duckdb::Value(connection_name),
	                                      duckdb::Value::UBIGINT(static_cast<uint64_t>(PREVIEW_MAX_ROWS))};
	const auto read = watched.Read(reader, request.limit, [&]() { return prepared->Submit(values); });
	watched.ThrowIfTimedOut();
	result.truncated = read.truncated;

	// §3.3 item 2: the schema the query's runs left is where the types start;
	// Preview reads it but does not write it.
	const auto known = ReadQuerySchema(connection, DefinitionHash(request.text));
	if (read.error.HasError()) {
		if (!IsFusionNoRows(read.error)) {
			throw FusionApiError(read.error);
		}
		// §5.2's schema for no rows, or none: Preview is not a run, so an
		// unknown schema is no error.
		for (const auto &column : known) {
			const auto *declared = Declared(header.columns, column.name);
			result.columns.push_back(Column(column.name, declared ? declared->type : column.type, declared));
		}
		if (result.columns.empty()) {
			for (const auto &declared : header.columns) {
				result.columns.push_back(Column(declared.name, declared.type, &declared));
			}
		}
		return;
	}

	std::vector<OracleColumnInference> inference;
	for (const auto &name : read.names) {
		const auto *declared = Declared(header.columns, name);
		const SchemaColumn *start = nullptr;
		for (const auto &column : known) {
			if (Lower(column.name) == Lower(name)) {
				start = &column;
			}
		}
		inference.emplace_back(name, declared ? &declared->type : nullptr, start ? &start->type : nullptr);
	}
	// "" for NULL: Oracle does not tell them apart (§3.3).
	const auto text = [](const duckdb::Value &value) { return value.IsNull() ? std::string() : value.ToString(); };
	for (const auto &row : read.rows) {
		for (size_t i = 0; i < row.size(); i++) {
			inference[i].Add(text(row[i]));
		}
	}
	for (size_t i = 0; i < read.names.size(); i++) {
		const auto finished = inference[i].Finish();
		if (!finished.ok) {
			throw ApiError(finished.code, 422, finished.message);
		}
		for (const auto &message : finished.warnings) {
			result.warnings.push_back({finished.widened ? WARNING_TYPE_WIDENED : WARNING_NUMBER_PRECISION, message});
		}
		result.columns.push_back(Column(read.names[i], finished.type, Declared(header.columns, read.names[i])));
	}
	for (const auto &row : read.rows) {
		std::vector<duckdb::Value> stored_row;
		for (size_t i = 0; i < row.size(); i++) {
			const auto &type = result.columns[i].type;
			const auto storage = StorageType(type);
			std::string stored;
			switch (ConvertOracleValue(type, text(row[i]), stored)) {
			case StoredValue::VALUE:
				stored_row.push_back(duckdb::Value(stored).DefaultCastAs(storage));
				break;
			case StoredValue::NULL_VALUE:
				stored_row.push_back(duckdb::Value(storage));
				break;
			case StoredValue::MISMATCH:
				// Inference saw every value, so this is a bug, not bad data.
				throw ApiError("E_INTERNAL", 500,
				               "value \"" + text(row[i]) + "\" does not fit type " + type.ToString() + " of column " +
				                   read.names[i]);
			}
		}
		result.rows.push_back(std::move(stored_row));
	}
}

//! An integer's value has 16 digits or more: |value| >= 10^15 (§3.4).
bool FromId(const duckdb::Value &value) {
	if (value.IsNull()) {
		return false;
	}
	const auto text = value.ToString();
	return text.size() - (text[0] == '-' ? 1 : 0) >= 16;
}

void PreviewLocal(duckdb::DatabaseInstance &db, duckdb::Connection &connection, const QueryHeader &header,
                  const PreviewRequest &request, Watched &watched, PreviewResult &result) {
	// Dependencies are read as they are: Preview runs none of them, and records
	// nothing in meta.missing (no workbook).
	RunRequest run;
	run.params = request.params;
	run.connection = request.connection;
	run.deps = DepsMode::NONE;
	Plan plan;
	PlanLocalRun(connection, header, run, plan);
	for (const auto &warning : plan.warnings) {
		result.warnings.push_back({warning.code, warning.message});
	}

	duckdb::Connection reader(db);
	CreateDependencyViews(reader, connection, plan.depends);
	auto prepared = reader.Prepare(plan.body);
	if (prepared->HasError()) {
		throw LocalSqlError(prepared->GetErrorObject());
	}
	auto named = BindLocalParams(*prepared, plan);
	const auto read = watched.Read(reader, request.limit, [&]() { return prepared->Submit(named); });
	watched.ThrowIfTimedOut();
	if (read.error.HasError()) {
		throw LocalSqlError(read.error);
	}
	result.truncated = read.truncated;

	for (size_t i = 0; i < read.names.size(); i++) {
		for (size_t j = 0; j < i; j++) {
			if (Lower(read.names[i]) == Lower(read.names[j])) {
				throw ApiError("E_SQL_COMPILE", 422,
				               "The result has two columns named " + read.names[i] + ": give them different names");
			}
		}
	}
	std::vector<const QueryColumn *> declared;
	for (size_t i = 0; i < read.names.size(); i++) {
		declared.push_back(Declared(header.columns, read.names[i]));
		if (declared.back()) {
			result.columns.push_back(Column(read.names[i], declared.back()->type, declared.back()));
			continue;
		}
		LocalColumnStats stats;
		if (IsIntegerType(read.types[i])) {
			for (const auto &row : read.rows) {
				stats.has_value_from_1e15 = stats.has_value_from_1e15 || FromId(row[i]);
			}
		}
		const auto mapping = MapLocalType(read.types[i], read.names[i], stats);
		if (mapping.cast_to_varchar) {
			result.warnings.push_back({WARNING_TYPE_WIDENED, mapping.warning});
		}
		result.columns.push_back(Column(read.names[i], mapping.type, nullptr));
	}
	for (size_t r = 0; r < read.rows.size(); r++) {
		std::vector<duckdb::Value> stored_row;
		for (size_t i = 0; i < read.names.size(); i++) {
			const auto storage = StorageType(result.columns[i].type);
			const auto &value = read.rows[r][i];
			if (declared[i]) {
				stored_row.push_back(DeclaredLocalValue(read.names[i], *declared[i], value, static_cast<int64_t>(r) + 1)
				                         .DefaultCastAs(storage));
			} else {
				// As a run stores it: the stage type first, then the storage type.
				stored_row.push_back(value.DefaultCastAs(LocalStageType(read.types[i])).DefaultCastAs(storage));
			}
		}
		result.rows.push_back(std::move(stored_row));
	}
}

} // namespace

PreviewResult RunPreview(duckdb::DatabaseInstance &db, const PreviewRequest &request) {
	const auto started = std::chrono::steady_clock::now();
	const auto parsed = ParseQueryFile(request.text);
	if (!parsed.Ok()) {
		throw ApiError(parsed.errors[0].code, 422, "The query cannot be parsed: " + parsed.errors[0].ToString());
	}
	const auto &header = parsed.header;
	duckdb::Connection connection(db);
	PreviewResult result;
	{
		Watched watched(RunTimeoutSetting(connection));
		if (header.engine == Engine::LOCAL) {
			PreviewLocal(db, connection, header, request, watched, result);
		} else {
			PreviewOracle(db, connection, header, request, watched, result);
		}
	}
	result.elapsed_ms =
	    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
	return result;
}

} // namespace ofexcel
