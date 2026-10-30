#include "ofexcel/run_engine.hpp"

#include "duckdb/common/types/uuid.hpp"
#include "duckdb/main/appender.hpp"
#include "duckdb/main/query_result_stream.hpp"
#include "ofexcel/api_error.hpp"
#include "ofexcel/connections.hpp"
#include "ofexcel/datasets.hpp"
#include "ofexcel/db_util.hpp"
#include "ofexcel/fusion_errors.hpp"
#include "ofexcel/hashing.hpp"
#include "ofexcel/oracle_render.hpp"
#include "ofexcel/query_file.hpp"
#include "ofexcel/result_types.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <functional>
#include <iostream>
#include <thread>
#include <vector>

namespace ofexcel {

//! A run that is going. The engine's maps and the run's own thread share it.
struct RunEngine::Active {
	std::string run_id;
	std::string instance_key;
	std::atomic<bool> cancel_requested {false};
	std::atomic<bool> timed_out {false};
	std::atomic<int64_t> rows {0};

	//! Guards `done` and `reader`.
	std::mutex mutex;
	std::condition_variable changed;
	bool done = false;
	//! The connection reading from ofquack, while it is.
	duckdb::Connection *reader = nullptr;
};

namespace {

//! §5.1, exactly. The SQL and the secret are parameters, never spliced in.
constexpr const char *ORACLE_QUERY_SQL =
    "SELECT * FROM oracle_fusion_query($1, secret := $2, all_varchar := true, on_cast_error := 'error', "
    "fetch_size := $3)";

//! How often a cancelled or timed-out run's reader is interrupted again. An
//! interrupt that lands before DuckDB starts the query is cleared when it does,
//! so one is not enough.
constexpr auto INTERRUPT_REPEAT = std::chrono::milliseconds(100);

constexpr const char *WARNING_PARAM_UNUSED = "W_PARAM_UNUSED";
constexpr const char *WARNING_TYPE_WIDENED = "W_TYPE_WIDENED";
constexpr const char *WARNING_NUMBER_PRECISION = "W_NUMBER_PRECISION";

struct Warning {
	std::string code;
	std::string message;
};

//! What Start settles before the run begins. The run only reads it.
struct Plan {
	std::string run_id;
	std::string query_id;
	std::string definition_hash;
	std::string instance_key;
	std::string connection;
	std::string rendered_sql;
	std::vector<QueryColumn> declared;
	int64_t max_rows = DEFAULT_MAX_ROWS;
	int64_t fetch_size = DEFAULT_FETCH_SIZE;
	int64_t timeout_seconds = DEFAULT_RUN_TIMEOUT_SECONDS;
	std::vector<Warning> warnings;
};

struct Outcome {
	std::string status = "failed";
	std::string error_code;
	std::string error_message;
	int64_t rows = 0;
	bool truncated = false;
	std::string result_table;
};

//! Thrown inside a run once it has been cancelled or has timed out.
struct Stopped {};

std::string Lower(std::string text) {
	std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::tolower(c); });
	return text;
}

duckdb::Value NullableString(const std::string &text) {
	return text.empty() ? duckdb::Value(duckdb::LogicalType::VARCHAR) : duckdb::Value(text);
}

duckdb::Value Now() {
	return duckdb::Value::TIMESTAMP(duckdb::Timestamp::GetCurrentTimestamp());
}

std::string WarningsJson(const std::vector<Warning> &warnings) {
	auto array = CanonicalJson::Array();
	for (const auto &warning : warnings) {
		auto item = CanonicalJson::Object();
		item.Set("code", CanonicalJson::String(warning.code)).Set("message", CanonicalJson::String(warning.message));
		array.Add(std::move(item));
	}
	return array.Serialize();
}

//! Interrupts the run's reader for as long as it is cancelled or past its
//! deadline, until the run is done.
void Watch(RunEngine::Active &run, int64_t timeout_seconds) {
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
	std::unique_lock<std::mutex> lock(run.mutex);
	while (!run.done) {
		const bool expired = std::chrono::steady_clock::now() >= deadline;
		if (!run.cancel_requested && !expired) {
			run.changed.wait_until(lock, deadline);
			continue;
		}
		if (!run.cancel_requested) {
			run.timed_out = true;
		}
		if (run.reader) {
			run.reader->Interrupt();
		}
		run.changed.wait_for(lock, INTERRUPT_REPEAT);
	}
}

//! One run, on three connections of its own: `reader` streams from ofquack and
//! is the one interrupted; `scanner` reads the stage table back; `writer`
//! creates tables, appends and writes meta.*. An open result belongs to its
//! connection, so a scan and an append cannot share one.
class OracleRun {
public:
	OracleRun(duckdb::DatabaseInstance &db, RunEngine::Active &run_p, const Plan &plan_p)
	    : run(run_p), plan(plan_p), reader(db), scanner(db), writer(db) {
		warnings = plan.warnings;
	}

	void Perform() {
		Outcome outcome;
		try {
			Fetch(outcome);
		} catch (const Stopped &) {
			StoppedOutcome(outcome);
		} catch (const ApiError &error) {
			outcome.status = "failed";
			outcome.error_code = error.code;
			outcome.error_message = error.what();
		} catch (const std::exception &exception) {
			const duckdb::ErrorData error(exception);
			if (Stopping()) {
				StoppedOutcome(outcome);
			} else {
				const auto mapped = FusionApiError(error);
				outcome.status = "failed";
				outcome.error_code = mapped.code;
				outcome.error_message = mapped.what();
			}
		}
		DropTable(stage);
		if (outcome.status != "succeeded") {
			RecordFailure(outcome);
		}
	}

private:
	bool Stopping() const {
		return run.cancel_requested || run.timed_out;
	}

	void ThrowIfStopping() const {
		if (Stopping()) {
			throw Stopped();
		}
	}

	void StoppedOutcome(Outcome &outcome) const {
		if (run.timed_out) {
			outcome.status = "failed";
			outcome.error_code = "E_TIMEOUT";
			outcome.error_message = "Запуск прерван: он шёл дольше " + std::to_string(plan.timeout_seconds) +
			                        " с (ofexcel_run_timeout)";
		} else {
			outcome.status = "cancelled";
			outcome.error_code = "E_CANCELLED";
			outcome.error_message = "Запуск отменён";
		}
	}

	void SetReader(duckdb::Connection *connection) {
		std::lock_guard<std::mutex> lock(run.mutex);
		run.reader = connection;
	}

	void Fetch(Outcome &outcome) {
		const auto error = ReadIntoStage(outcome);
		if (error.HasError()) {
			if (Stopping()) {
				throw Stopped();
			}
			if (!IsFusionNoRows(error)) {
				throw FusionApiError(error);
			}
			StoreEmpty(outcome);
			return;
		}
		ThrowIfStopping();
		StoreRows(outcome);
	}

	//! §5.1: streams the result into ds.<stage>, every column VARCHAR, keeping
	//! at most max_rows rows (§5.4). Returns ofquack's error, if any, with its
	//! type intact for §14.1.
	duckdb::ErrorData ReadIntoStage(Outcome &outcome) {
		auto prepared = reader.Prepare(ORACLE_QUERY_SQL);
		if (prepared->HasError()) {
			return prepared->GetErrorObject();
		}
		duckdb::vector<duckdb::Value> values {duckdb::Value(plan.rendered_sql), duckdb::Value(plan.connection),
		                                      duckdb::Value::UBIGINT(static_cast<uint64_t>(plan.fetch_size))};
		SetReader(&reader);
		// Cleared on every way out, so the watchdog never interrupts a connection
		// that is gone.
		struct ClearReader {
			OracleRun &self;
			~ClearReader() {
				self.SetReader(nullptr);
			}
		} clear_reader {*this};

		auto handle = prepared->Submit(values);
		if (handle->HasError()) {
			return handle->GetErrorObject();
		}
		duckdb::QueryResultStream<> stream(std::move(handle));
		for (const auto &name : stream.GetNames()) {
			names.push_back(name.GetIdentifierName());
		}

		std::string columns;
		for (size_t i = 0; i < names.size(); i++) {
			columns += (i ? ", c" : "c") + std::to_string(i) + " VARCHAR";
		}
		stage = StageTableName(plan.run_id);
		Execute(writer, "CREATE TABLE ds." + QuoteIdentifier(stage) + " (" + columns + ")");
		duckdb::Appender appender(writer, duckdb::Identifier(DATASET_SCHEMA), duckdb::Identifier(stage));

		duckdb::ErrorData error;
		int64_t kept = 0;
		while (true) {
			duckdb::unique_ptr<duckdb::DataChunk> chunk;
			try {
				chunk = stream.Fetch();
			} catch (const std::exception &exception) {
				error = duckdb::ErrorData(exception);
				break;
			}
			if (!chunk) {
				break;
			}
			if (chunk->size() == 0) {
				continue;
			}
			// Row max_rows + 1 is what tells a truncated result from one that
			// fits exactly (§5.4).
			const auto room = plan.max_rows - kept;
			if (room <= 0) {
				outcome.truncated = true;
				break;
			}
			if (static_cast<int64_t>(chunk->size()) > room) {
				chunk->SetCardinality(static_cast<duckdb::idx_t>(room));
				outcome.truncated = true;
			}
			appender.AppendDataChunk(*chunk);
			kept += static_cast<int64_t>(chunk->size());
			run.rows = kept;
			if (outcome.truncated) {
				break;
			}
		}
		if (outcome.truncated) {
			// The rest is not needed: stop ofquack between pages, or in one.
			reader.Interrupt();
			stream.Close();
		} else if (!error.HasError() && stream.HasError()) {
			error = stream.GetErrorObject();
		}
		appender.Close();
		outcome.rows = kept;
		return error;
	}

	//! Calls `visit` with every chunk of the stage table, in the order the rows
	//! were appended.
	void ScanStage(const std::function<void(duckdb::DataChunk &)> &visit) {
		auto handle = scanner.Submit("SELECT * FROM ds." + QuoteIdentifier(stage));
		if (handle->HasError()) {
			handle->GetErrorObject().Throw();
		}
		duckdb::QueryResultStream<> stream(std::move(handle));
		while (auto chunk = stream.Fetch()) {
			ThrowIfStopping();
			visit(*chunk);
		}
		if (stream.HasError()) {
			stream.GetErrorObject().Throw();
		}
	}

	//! The text of every value of `column`, "" for NULL: Oracle does not tell
	//! them apart, and ofquack sends NULL as "" anyway (§3.3).
	static void ForEachText(duckdb::DataChunk &chunk, duckdb::idx_t column,
	                        const std::function<void(duckdb::idx_t, const std::string &)> &visit) {
		duckdb::UnifiedVectorFormat format;
		chunk.data[column].ToUnifiedFormat(format);
		const auto *strings = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(format);
		const std::string empty;
		for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
			const auto index = format.sel->get_index(row);
			if (format.validity.RowIsValid(index)) {
				visit(row, strings[index].GetString());
			} else {
				visit(row, empty);
			}
		}
	}

	const QueryColumn *Declared(const std::string &name) const {
		const auto lower = Lower(name);
		for (const auto &column : plan.declared) {
			if (Lower(column.name) == lower) {
				return &column;
			}
		}
		return nullptr;
	}

	static const SchemaColumn *Known(const std::vector<SchemaColumn> &known, const std::string &name) {
		const auto lower = Lower(name);
		for (const auto &column : known) {
			if (Lower(column.name) == lower) {
				return &column;
			}
		}
		return nullptr;
	}

	//! §3.3 over the whole stage table.
	std::vector<SchemaColumn> InferSchema(const std::vector<SchemaColumn> &known) {
		std::vector<OracleColumnInference> inference;
		for (const auto &name : names) {
			const auto *declared = Declared(name);
			const auto *start = Known(known, name);
			inference.emplace_back(name, declared ? &declared->type : nullptr, start ? &start->type : nullptr);
		}
		ScanStage([&](duckdb::DataChunk &chunk) {
			for (duckdb::idx_t column = 0; column < chunk.ColumnCount(); column++) {
				auto &column_inference = inference[column];
				ForEachText(chunk, column, [&](duckdb::idx_t, const std::string &text) { column_inference.Add(text); });
			}
		});

		std::vector<SchemaColumn> schema;
		for (size_t i = 0; i < names.size(); i++) {
			const auto finished = inference[i].Finish();
			if (!finished.ok) {
				throw ApiError(finished.code, 500, finished.message);
			}
			for (const auto &message : finished.warnings) {
				warnings.push_back({finished.widened ? WARNING_TYPE_WIDENED : WARNING_NUMBER_PRECISION, message});
			}
			const auto *declared = Declared(names[i]);
			schema.push_back({names[i], finished.type, declared ? declared->header : ""});
		}
		return schema;
	}

	//! The instance's next version number: one past its current one.
	int64_t NextVersion() {
		const auto rows = QueryRows(writer,
		                            "SELECT current_version FROM meta.dataset_instances WHERE instance_key = $1",
		                            {duckdb::Value(plan.instance_key)});
		return (rows.empty() ? 0 : rows[0][0].GetValue<int64_t>()) + 1;
	}

	void CreateTarget(const std::vector<SchemaColumn> &schema, Outcome &outcome) {
		const auto version = NextVersion();
		std::string columns;
		for (size_t i = 0; i < schema.size(); i++) {
			columns += (i ? ", " : "") + QuoteIdentifier(schema[i].name) + " " + StorageType(schema[i].type).ToString();
		}
		const auto table = InstanceTableName(plan.query_id, plan.instance_key, version);
		Execute(writer, "CREATE TABLE ds." + QuoteIdentifier(table) + " (" + columns + ")");
		outcome.result_table = table;
		target_version = version;
	}

	void StoreRows(Outcome &outcome) {
		const auto schema = InferSchema(ReadQuerySchema(writer, plan.definition_hash));
		CreateTarget(schema, outcome);
		duckdb::Appender appender(writer, duckdb::Identifier(DATASET_SCHEMA), duckdb::Identifier(outcome.result_table));
		ScanStage([&](duckdb::DataChunk &chunk) {
			std::vector<std::vector<duckdb::Value>> rows(chunk.size(), std::vector<duckdb::Value>(schema.size()));
			for (duckdb::idx_t column = 0; column < chunk.ColumnCount(); column++) {
				const auto &type = schema[column].type;
				ForEachText(chunk, column, [&](duckdb::idx_t row, const std::string &text) {
					std::string stored;
					switch (ConvertOracleValue(type, text, stored)) {
					case StoredValue::VALUE:
						rows[row][column] = duckdb::Value(stored);
						break;
					case StoredValue::NULL_VALUE:
						break;
					case StoredValue::MISMATCH:
						// Inference saw every value, so this is a bug, not bad data.
						throw ApiError("E_INTERNAL", 500,
						               "значение «" + text + "» не подходит к типу " + type.ToString() + " колонки " +
						                   schema[column].name);
					}
				});
			}
			for (auto &row : rows) {
				appender.BeginRow();
				for (auto &value : row) {
					appender.Append(std::move(value));
				}
				appender.EndRow();
			}
		});
		appender.Close();
		ThrowIfStopping();
		Commit(outcome, schema, true);
	}

	//! §5.2: an empty result keeps the schema the query is known to have.
	void StoreEmpty(Outcome &outcome) {
		auto schema = ReadQuerySchema(writer, plan.definition_hash);
		for (auto &column : schema) {
			if (const auto *declared = Declared(column.name)) {
				column.type = declared->type;
				column.header = declared->header;
			}
		}
		if (schema.empty()) {
			for (const auto &declared : plan.declared) {
				schema.push_back({declared.name, declared.type, declared.header});
			}
		}
		if (schema.empty()) {
			throw ApiError("E_EMPTY_UNKNOWN_SCHEMA", 500,
			               "Запрос вернул 0 строк, а схема неизвестна. Объявите колонки через @column или выполните "
			               "запрос с параметрами, при которых есть строки");
		}
		outcome.rows = 0;
		outcome.truncated = false;
		CreateTarget(schema, outcome);
		ThrowIfStopping();
		Commit(outcome, schema, false);
	}

	void UpdateRun(const Outcome &outcome) {
		QueryRows(writer,
		          "UPDATE meta.runs SET status = $2, finished_at = $3, rows_fetched = $4, truncated = $5, "
		          "result_table = $6, warnings_json = $7, error_code = $8, error_message = $9 WHERE run_id = $1",
		          {duckdb::Value(plan.run_id), duckdb::Value(outcome.status), Now(),
		           duckdb::Value::BIGINT(outcome.rows), duckdb::Value::BOOLEAN(outcome.truncated),
		           NullableString(outcome.result_table), duckdb::Value(WarningsJson(warnings)),
		           NullableString(outcome.error_code), NullableString(outcome.error_message)});
	}

	//! §6.4: the new version becomes current, the one before it previous, and
	//! the one before that is dropped, in one transaction.
	void Commit(Outcome &outcome, const std::vector<SchemaColumn> &schema, bool write_schema) {
		outcome.status = "succeeded";
		std::lock_guard<std::mutex> guard(MetaWriteLock());
		InTransaction(writer, [&]() {
			const auto previous =
			    QueryRows(writer, "SELECT prev_table FROM meta.dataset_instances WHERE instance_key = $1",
			              {duckdb::Value(plan.instance_key)});
			if (!previous.empty() && !previous[0][0].IsNull()) {
				Execute(writer, "DROP TABLE IF EXISTS ds." + QuoteIdentifier(previous[0][0].ToString()));
			}
			const auto now = Now();
			QueryRows(writer,
			          "UPDATE meta.dataset_instances SET prev_table = current_table, current_table = $2, "
			          "current_version = $3, data_version = data_version + 1, fetched_at = $4, row_count = $5, "
			          "truncated = $6, status = 'ready', last_error_code = NULL, last_error = NULL, "
			          "last_access_at = $4 "
			          "WHERE instance_key = $1",
			          {duckdb::Value(plan.instance_key), duckdb::Value(outcome.result_table),
			           duckdb::Value::INTEGER(static_cast<int32_t>(target_version)), now,
			           duckdb::Value::BIGINT(outcome.rows), duckdb::Value::BOOLEAN(outcome.truncated)});
			Execute(writer, "UPDATE meta.state SET value = CAST(CAST(value AS BIGINT) + 1 AS VARCHAR) "
			                "WHERE key = 'data_version'");
			if (write_schema) {
				WriteQuerySchema(writer, plan.definition_hash, schema);
			}
			UpdateRun(outcome);
		});
	}

	//! A failed or cancelled run: its own table goes, the instance keeps its
	//! current version and learns why (§6.4).
	void RecordFailure(Outcome &outcome) {
		DropTable(outcome.result_table);
		outcome.result_table.clear();
		try {
			std::lock_guard<std::mutex> guard(MetaWriteLock());
			InTransaction(writer, [&]() {
				QueryRows(writer,
				          "UPDATE meta.dataset_instances SET last_error_code = $2, last_error = $3 "
				          "WHERE instance_key = $1",
				          {duckdb::Value(plan.instance_key), duckdb::Value(outcome.error_code),
				           duckdb::Value(outcome.error_message)});
				UpdateRun(outcome);
			});
		} catch (const std::exception &error) {
			std::cerr << "ofexceld: run " << plan.run_id << " not recorded: " << duckdb::ErrorData(error).Message()
			          << std::endl;
		}
	}

	void DropTable(const std::string &table) {
		if (table.empty()) {
			return;
		}
		try {
			Execute(writer, "DROP TABLE IF EXISTS ds." + QuoteIdentifier(table));
		} catch (const std::exception &error) {
			std::cerr << "ofexceld: table ds." << table << " not dropped: " << duckdb::ErrorData(error).Message()
			          << std::endl;
		}
	}

	RunEngine::Active &run;
	const Plan &plan;
	duckdb::Connection reader;
	duckdb::Connection scanner;
	duckdb::Connection writer;
	std::vector<Warning> warnings;
	//! The result's column names, as the report named them.
	std::vector<std::string> names;
	std::string stage;
	int64_t target_version = 0;
};

int64_t RunTimeoutSetting(duckdb::Connection &connection) {
	const auto outcome = TryQueryRows(connection, "SELECT current_setting('ofexcel_run_timeout')", {});
	if (!outcome.Ok() || outcome.rows.empty() || outcome.rows[0][0].IsNull()) {
		return DEFAULT_RUN_TIMEOUT_SECONDS;
	}
	const auto seconds = outcome.rows[0][0].GetValue<int64_t>();
	return seconds > 0 ? seconds : DEFAULT_RUN_TIMEOUT_SECONDS;
}

duckdb::Value Environment(duckdb::Connection &connection, const std::string &name) {
	const auto rows =
	    QueryRows(connection, "SELECT environment FROM meta.connections WHERE name = $1", {duckdb::Value(name)});
	return rows.empty() ? duckdb::Value(duckdb::LogicalType::VARCHAR) : rows[0][0];
}

//! The Fusion user ofquack signed in as, or NULL when it does not know.
duckdb::Value FusionUser(duckdb::Connection &connection, const std::string &name) {
	const auto outcome = TryQueryRows(connection, "SELECT subject FROM fusion_scanner_sso_status(secret := $1)",
	                                  {duckdb::Value(name)});
	if (!outcome.Ok() || outcome.rows.empty() || outcome.rows[0][0].IsNull() ||
	    outcome.rows[0][0].ToString().empty()) {
		return duckdb::Value(duckdb::LogicalType::VARCHAR);
	}
	return outcome.rows[0][0];
}

} // namespace

RunEngine &RunEngine::Get() {
	static RunEngine engine;
	return engine;
}

RunEngine::~RunEngine() {
	std::unique_lock<std::mutex> lock(mutex);
	for (auto &entry : by_run) {
		entry.second->cancel_requested = true;
		entry.second->changed.notify_all();
	}
	idle.wait(lock, [&]() { return by_run.empty(); });
}

RunStart RunEngine::Start(duckdb::DatabaseInstance &db, const RunRequest &request) {
	const auto &definition = request.definition;
	const auto parsed = ParseQueryFile(definition.text);
	if (!parsed.Ok()) {
		throw ApiError(parsed.errors[0].code, 422,
		               "Запрос " + definition.query_id + " не разбирается: " + parsed.errors[0].ToString());
	}
	const auto &header = parsed.header;
	if (header.engine == Engine::LOCAL) {
		throw ApiError("E_UNSUPPORTED", 422, "Local-запросы пока не выполняются: " + header.id);
	}
	const auto connection_name = request.connection.empty() ? header.connection : request.connection;
	if (connection_name.empty()) {
		throw ApiError("E_BAD_REQUEST", 422,
		               "Не задано подключение: передайте connection или объявите @connection в запросе " + header.id);
	}
	duckdb::Connection connection(db);
	if (!FusionSecretExists(connection, connection_name)) {
		throw ApiError("E_NOT_FOUND", 404, "Нет подключения " + connection_name);
	}
	const auto rendered = RenderOracle(header.body, header.params, request.params);
	if (!rendered.ok) {
		throw ApiError(rendered.code, 422, rendered.message);
	}

	auto plan = std::make_shared<Plan>();
	plan->query_id = header.id;
	plan->definition_hash = DefinitionHash(definition.text);
	plan->connection = connection_name;
	plan->rendered_sql = rendered.rendered;
	plan->declared = header.columns;
	plan->max_rows = header.max_rows > 0 ? header.max_rows : DEFAULT_MAX_ROWS;
	plan->timeout_seconds = RunTimeoutSetting(connection);
	for (const auto &message : rendered.warnings) {
		plan->warnings.push_back({WARNING_PARAM_UNUSED, message});
	}
	const auto params_json = CanonicalParams(rendered.values);
	plan->instance_key = InstanceKey(header.id, plan->definition_hash, connection_name, params_json);
	const auto environment = Environment(connection, connection_name);
	const auto user = FusionUser(connection, connection_name);

	std::lock_guard<std::mutex> lock(mutex);
	const auto going = by_instance.find(plan->instance_key);
	if (going != by_instance.end()) {
		if (request.force) {
			throw ApiError("E_CONFLICT", 409,
			               "Этот набор данных уже обновляется (запуск " + going->second + "), дождитесь его окончания");
		}
		return {going->second, true};
	}
	plan->run_id = duckdb::UUID::ToString(duckdb::UUID::GenerateRandomUUID());
	{
		std::lock_guard<std::mutex> meta(MetaWriteLock());
		InTransaction(connection, [&]() {
			const auto now = Now();
			// §6.3: an instance exists from its first run on, with status 'empty'
			// until one succeeds.
			QueryRows(connection,
			          "INSERT INTO meta.dataset_instances (instance_key, query_id, definition_hash, connection_name, "
			          "params_json, status, last_access_at) VALUES ($1, $2, $3, $4, $5, 'empty', $6) "
			          "ON CONFLICT (instance_key) DO UPDATE SET last_access_at = excluded.last_access_at",
			          {duckdb::Value(plan->instance_key), duckdb::Value(plan->query_id),
			           duckdb::Value(plan->definition_hash), duckdb::Value(connection_name), duckdb::Value(params_json),
			           now});
			QueryRows(connection,
			          "INSERT INTO meta.runs (run_id, instance_key, query_id, definition_hash, connection_name, "
			          "environment, params_json, workbook_id, parent_run_id, user_name, status, started_at, "
			          "warnings_json) VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, 'running', $11, $12)",
			          {duckdb::Value(plan->run_id), duckdb::Value(plan->instance_key), duckdb::Value(plan->query_id),
			           duckdb::Value(plan->definition_hash), duckdb::Value(connection_name), environment,
			           duckdb::Value(params_json), NullableString(request.workbook_id),
			           NullableString(request.parent_run_id), user, now, duckdb::Value(WarningsJson(plan->warnings))});
		});
	}

	auto active = std::make_shared<Active>();
	active->run_id = plan->run_id;
	active->instance_key = plan->instance_key;
	by_run[plan->run_id] = active;
	by_instance[plan->instance_key] = plan->run_id;

	auto shared_db = db.shared_from_this();
	std::thread([this, shared_db, active, plan]() {
		{
			std::thread watchdog([&]() { Watch(*active, plan->timeout_seconds); });
			try {
				OracleRun(*shared_db, *active, *plan).Perform();
			} catch (const std::exception &error) {
				std::cerr << "ofexceld: run " << plan->run_id << " failed outside its own handling: "
				          << duckdb::ErrorData(error).Message() << std::endl;
			}
			{
				std::lock_guard<std::mutex> guard(active->mutex);
				active->done = true;
			}
			active->changed.notify_all();
			watchdog.join();
		}
		Finished(plan->run_id, plan->instance_key);
	}).detach();
	return {plan->run_id, false};
}

void RunEngine::Finished(const std::string &run_id, const std::string &instance_key) {
	std::lock_guard<std::mutex> lock(mutex);
	by_run.erase(run_id);
	by_instance.erase(instance_key);
	idle.notify_all();
}

void RunEngine::Wait(const std::string &run_id) {
	std::unique_lock<std::mutex> lock(mutex);
	idle.wait(lock, [&]() { return by_run.find(run_id) == by_run.end(); });
}

bool RunEngine::Cancel(const std::string &run_id) {
	std::lock_guard<std::mutex> lock(mutex);
	const auto found = by_run.find(run_id);
	if (found == by_run.end()) {
		return false;
	}
	found->second->cancel_requested = true;
	{
		std::lock_guard<std::mutex> guard(found->second->mutex);
	}
	found->second->changed.notify_all();
	return true;
}

int64_t RunEngine::RowsFetched(const std::string &run_id) {
	std::lock_guard<std::mutex> lock(mutex);
	const auto found = by_run.find(run_id);
	return found == by_run.end() ? -1 : found->second->rows.load();
}

size_t RunEngine::ActiveRuns() {
	std::lock_guard<std::mutex> lock(mutex);
	return by_run.size();
}

} // namespace ofexcel
