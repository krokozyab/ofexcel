// Runs and datasets over HTTP (CONTRACTS.md §8.3; TASKS.md C4, C6): starting a
// run, following and cancelling it, reading its rows, the dataset instances runs
// leave behind, and Preview. The runs themselves are src/run/run_engine.cpp,
// Preview is src/run/preview.cpp.

#include "routes.hpp"

#include "ofexcel/api_error.hpp"
#include "ofexcel/contract_types.hpp"
#include "ofexcel/datasets.hpp"
#include "ofexcel/db_util.hpp"
#include "ofexcel/json_body.hpp"
#include "ofexcel/param_values.hpp"
#include "ofexcel/preview.hpp"
#include "ofexcel/query_registry.hpp"
#include "ofexcel/result_types.hpp"
#include "ofexcel/run_engine.hpp"

#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/timestamp.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace ofexcel {

namespace {

constexpr int64_t DEFAULT_PAGE_ROWS = 5000;
constexpr int64_t MAX_PAGE_ROWS = 20000;
constexpr int64_t DEFAULT_RUNS_LISTED = 50;
constexpr int64_t MAX_RUNS_LISTED = 1000;
//! §8.3: a cancelled run is cancelled within 5 s; the route waits that long
//! so that its answer already says so.
constexpr auto CANCEL_WAIT = std::chrono::seconds(5);

//! depends_on: the runs started for this one, oldest first (§8.2 Run).
const char *const RUN_COLUMNS =
    "SELECT run_id, instance_key, query_id, definition_hash, connection_name, environment, params_json, "
    "parent_run_id, status, started_at, finished_at, rows_fetched, truncated, warnings_json, error_code, "
    "error_message, result_table, (SELECT string_agg(c.run_id, ',' ORDER BY c.started_at, c.run_id) "
    "FROM meta.runs AS c WHERE c.parent_run_id = r.run_id) FROM meta.runs AS r ";

enum RunColumn {
	RUN_ID,
	INSTANCE_KEY,
	QUERY_ID,
	DEFINITION_HASH,
	CONNECTION_NAME,
	ENVIRONMENT,
	PARAMS_JSON,
	PARENT_RUN_ID,
	STATUS,
	STARTED_AT,
	FINISHED_AT,
	ROWS_FETCHED,
	TRUNCATED,
	WARNINGS_JSON,
	ERROR_CODE,
	ERROR_MESSAGE,
	RESULT_TABLE,
	DEPENDS_ON
};

std::string UrlParam(const httplib::Request &request, const char *name) {
	return request.has_param(name) ? request.get_param_value(name) : "";
}

//! A non-negative integer URL parameter within [minimum, maximum], or
//! `fallback` when it is absent. Anything else is E_BAD_REQUEST (422).
int64_t IntegerParam(const httplib::Request &request, const char *name, int64_t fallback, int64_t minimum,
                     int64_t maximum) {
	const auto text = UrlParam(request, name);
	if (text.empty()) {
		return fallback;
	}
	const bool digits =
	    text.size() <= 9 && std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isdigit(c); });
	const auto value = digits ? std::stoll(text) : -1;
	if (value < minimum || value > maximum) {
		throw ApiError("E_BAD_REQUEST", 422,
		               std::string(name) + " must be an integer from " + std::to_string(minimum) + " to " +
		                   std::to_string(maximum) + ", not \"" + text + "\"");
	}
	return value;
}

//! A TIMESTAMP from meta.* as §1 writes time in JSON: YYYY-MM-DDTHH:MM:SSZ.
std::string IsoTime(const duckdb::Value &value) {
	duckdb::date_t date;
	duckdb::dtime_t time;
	duckdb::Timestamp::Convert(value.GetValue<duckdb::timestamp_t>(), date, time);
	int32_t year, month, day, hour, minute, second, micros;
	duckdb::Date::Convert(date, year, month, day);
	duckdb::Time::Convert(time, hour, minute, second, micros);
	char text[32];
	std::snprintf(text, sizeof(text), "%04d-%02d-%02dT%02d:%02d:%02dZ", year, month, day, hour, minute, second);
	return text;
}

//! canonical_params (§1.2) back as Record<string, ParamValue>: canonical
//! strings, and arrays of them for list<T>.
JsonObject ParamsJson(const std::string &params_json) {
	JsonObject params;
	const auto document = JsonDocument::Parse(params_json);
	const auto root = document.Root();
	for (const auto &key : root.Keys()) {
		const auto value = root.Get(key);
		if (value.Is(JsonKind::ARRAY)) {
			auto elements = params.SetArray(key);
			for (size_t i = 0; i < value.Size(); i++) {
				elements.AddString(value.At(i).String());
			}
		} else {
			params.Set(key, value.String());
		}
	}
	return params;
}

JsonObject QueryRef(const std::string &id, const std::string &hash) {
	JsonObject query;
	query.Set("id", id).Set("hash", hash);
	return query;
}

JsonObject ErrorJson(const std::string &code, const std::string &message) {
	JsonObject error;
	error.Set("code", code).Set("message", message);
	return error;
}

//! A meta.runs row as §8.2's Run. A run that is going reports the rows it has
//! read so far, which meta.runs only gets at the end.
JsonObject RunJson(const Row &row) {
	const auto run_id = row[RUN_ID].ToString();
	const auto status = row[STATUS].ToString();
	auto rows_fetched = row[ROWS_FETCHED].GetValue<int64_t>();
	if (status == "running" || status == "queued") {
		rows_fetched = std::max(rows_fetched, RunEngine::Get().RowsFetched(run_id));
	}
	JsonObject run;
	run.Set("run_id", run_id)
	    .Set("instance_key", row[INSTANCE_KEY].ToString())
	    .Set("query", QueryRef(row[QUERY_ID].ToString(), row[DEFINITION_HASH].ToString()))
	    .Set("connection", row[CONNECTION_NAME].ToString());
	if (!row[ENVIRONMENT].IsNull()) {
		run.Set("environment", row[ENVIRONMENT].ToString());
	}
	run.Set("params", ParamsJson(row[PARAMS_JSON].ToString()))
	    .Set("status", status)
	    .Set("started_at", IsoTime(row[STARTED_AT]));
	if (!row[FINISHED_AT].IsNull()) {
		run.Set("finished_at", IsoTime(row[FINISHED_AT]));
	}
	run.Set("rows_fetched", rows_fetched).Set("truncated", row[TRUNCATED].GetValue<bool>());
	if (!row[PARENT_RUN_ID].IsNull()) {
		run.Set("parent_run_id", row[PARENT_RUN_ID].ToString());
	}
	auto depends_on = run.SetArray("depends_on");
	if (!row[DEPENDS_ON].IsNull()) {
		const auto ids = row[DEPENDS_ON].ToString();
		for (size_t from = 0; from <= ids.size();) {
			const auto comma = std::min(ids.find(',', from), ids.size());
			depends_on.AddString(ids.substr(from, comma - from));
			from = comma + 1;
		}
	}
	auto warnings = run.SetArray("warnings");
	const auto stored = JsonDocument::Parse(row[WARNINGS_JSON].ToString());
	for (size_t i = 0; i < stored.Root().Size(); i++) {
		const auto warning = stored.Root().At(i);
		warnings.AddObject(ErrorJson(warning.Get("code").String(), warning.Get("message").String()));
	}
	if (!row[ERROR_CODE].IsNull()) {
		run.Set("error", ErrorJson(row[ERROR_CODE].ToString(),
		                           row[ERROR_MESSAGE].IsNull() ? "" : row[ERROR_MESSAGE].ToString()));
	}
	return run;
}

Row FindRun(duckdb::Connection &connection, const std::string &run_id) {
	auto rows = QueryRows(connection, std::string(RUN_COLUMNS) + "WHERE r.run_id = $1", {duckdb::Value(run_id)});
	if (rows.empty()) {
		throw ApiError("E_NOT_FOUND", 404, "No run " + run_id);
	}
	return std::move(rows[0]);
}

//! Reading an instance's rows keeps it from expiring (ofexcel_dataset_ttl_days).
void TouchInstance(duckdb::Connection &connection, const std::string &instance_key) {
	std::lock_guard<std::mutex> guard(MetaWriteLock());
	QueryRows(connection, "UPDATE meta.dataset_instances SET last_access_at = $2 WHERE instance_key = $1",
	          {duckdb::Value(instance_key), duckdb::Value::TIMESTAMP(duckdb::Timestamp::GetCurrentTimestamp())});
}

std::string Lower(std::string text) {
	std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::tolower(c); });
	return text;
}

struct PageColumn {
	std::string name;
	std::string header;
	ColumnType type;
};

//! The contract type of a stored column when meta.query_schemas does not say:
//! the schema there is the latest run's, and an older version may have a
//! narrower type (§3.3 item 4).
ColumnType TypeOfStorage(const std::string &data_type) {
	auto lower = Lower(data_type);
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

//! The columns of ds.<table> with their contract types: from the definition's
//! schema where it matches how the column is stored, from the storage type
//! otherwise. Empty when the table is gone.
std::vector<PageColumn> TableColumns(duckdb::Connection &connection, const std::string &table,
                                     const std::string &definition_hash) {
	const auto stored = QueryRows(connection,
	                              "SELECT column_name, data_type FROM duckdb_columns() WHERE database_name = "
	                              "current_database() AND schema_name = 'ds' AND table_name = $1 ORDER BY column_index",
	                              {duckdb::Value(table)});
	const auto schema = ReadQuerySchema(connection, definition_hash);
	std::vector<PageColumn> columns;
	for (const auto &row : stored) {
		PageColumn column;
		column.name = row[0].ToString();
		const auto data_type = row[1].ToString();
		column.type = TypeOfStorage(data_type);
		column.header = column.name;
		for (const auto &known : schema) {
			if (Lower(known.name) == Lower(column.name) && StorageType(known.type).ToString() == data_type) {
				column.type = known.type;
				if (!known.header.empty()) {
					column.header = known.header;
				}
				break;
			}
		}
		columns.push_back(std::move(column));
	}
	return columns;
}

void AddCell(JsonArrayRef &row, const ColumnType &type, const duckdb::Value &value) {
	const auto text = value.IsNull() ? std::string() : value.ToString();
	const auto cell = EncodeJsonValue(type, value.IsNull() ? nullptr : &text);
	switch (cell.kind) {
	case JsonCell::Kind::NULL_VALUE:
		row.AddNull();
		break;
	case JsonCell::Kind::STRING:
		row.AddString(cell.text);
		break;
	case JsonCell::Kind::INTEGER:
		row.AddInt(cell.integer);
		break;
	case JsonCell::Kind::NUMBER:
		row.AddDouble(cell.number);
		break;
	case JsonCell::Kind::BOOLEAN:
		row.AddBool(cell.boolean);
		break;
	}
}

//! §8.2's RowsPage over ds.<table>, in the order the rows were stored. A table
//! that is gone -- a newer version replaced it twice over -- is E_GONE (410).
void SendRowsPage(const httplib::Request &request, httplib::Response &response, duckdb::Connection &connection,
                  const std::string &table, const std::string &definition_hash, bool truncated) {
	const auto offset = IntegerParam(request, "offset", 0, 0, 999999999);
	const auto limit = IntegerParam(request, "limit", DEFAULT_PAGE_ROWS, 1, MAX_PAGE_ROWS);
	const auto gone = [&]() {
		return ApiError("E_GONE", 410,
		                "This version of the result is gone: a newer run replaced it. Run the query again");
	};
	const auto columns = TableColumns(connection, table, definition_hash);
	if (columns.empty()) {
		throw gone();
	}
	const auto quoted = "ds." + QuoteIdentifier(table);
	const auto total = TryQueryRows(connection, "SELECT count(*) FROM " + quoted, {});
	const auto page = TryQueryRows(connection, "SELECT * FROM " + quoted + " ORDER BY rowid LIMIT $1 OFFSET $2",
	                               {duckdb::Value::BIGINT(limit), duckdb::Value::BIGINT(offset)});
	if (!total.Ok() || !page.Ok()) {
		// Dropped between the column lookup and the read.
		throw gone();
	}

	JsonObject body;
	auto infos = body.SetArray("columns");
	for (const auto &column : columns) {
		JsonObject info;
		info.Set("name", column.name).Set("header", column.header).Set("type", column.type.ToString());
		infos.AddObject(info);
	}
	auto rows = body.SetArray("rows");
	for (const auto &stored : page.rows) {
		auto row = rows.AddArray();
		for (size_t i = 0; i < columns.size() && i < stored.size(); i++) {
			AddCell(row, columns[i].type, stored[i]);
		}
	}
	body.Set("offset", offset)
	    .Set("limit", limit)
	    .Set("total", total.rows[0][0].GetValue<int64_t>())
	    .Set("truncated", truncated);
	SendJson(response, 200, body.Serialize());
}

// --- /v1/runs ------------------------------------------------------------------

//! A parameter value as §3.1 takes it from JSON; numbers keep their text.
ParamInput ToParamInput(const std::string &name, const JsonValue &value) {
	switch (value.Kind()) {
	case JsonKind::MISSING:
		return ParamInput::Missing();
	case JsonKind::NULL_VALUE:
		return ParamInput::Null();
	case JsonKind::BOOLEAN:
		return ParamInput::Boolean(value.Bool());
	case JsonKind::NUMBER:
		return ParamInput::Number(value.NumberText());
	case JsonKind::STRING:
		return ParamInput::String(value.String());
	case JsonKind::ARRAY: {
		std::vector<ParamInput> elements;
		for (size_t i = 0; i < value.Size(); i++) {
			elements.push_back(ToParamInput(name, value.At(i)));
		}
		return ParamInput::Array(std::move(elements));
	}
	case JsonKind::OBJECT:
		break;
	}
	throw ApiError("E_PARAM_TYPE", 422, "parameter " + name + ": an object is not a parameter value");
}

std::string OptionalString(const JsonValue &body, const char *key) {
	const auto value = body.Get(key);
	if (value.Is(JsonKind::MISSING) || value.Is(JsonKind::NULL_VALUE)) {
		return "";
	}
	if (!value.Is(JsonKind::STRING)) {
		throw ApiError("E_BAD_REQUEST", 422, std::string(key) + " must be a string");
	}
	return value.String();
}

void StartRunRoute(const httplib::Request &request, httplib::Response &response, duckdb::Connection &connection) {
	const auto document = ParseBody(request);
	const auto body = document.Root();
	const auto query = body.Get("query");
	if (!query.Is(JsonKind::OBJECT)) {
		throw ApiError("E_BAD_REQUEST", 422, "The request body has no query object {id, hash}");
	}
	const auto query_id = RequiredString(query, "id");
	const auto hash = RequiredString(query, "hash");

	RunRequest run;
	run.connection = OptionalString(body, "connection");
	run.workbook_id = OptionalString(body, "workbook_id");
	const auto deps = OptionalString(body, "deps");
	if (deps == "all") {
		run.deps = DepsMode::ALL;
	} else if (deps == "none") {
		run.deps = DepsMode::NONE;
	} else if (!deps.empty() && deps != "missing") {
		throw ApiError("E_BAD_REQUEST", 422, "deps must be missing, all or none, not \"" + deps + "\"");
	}
	const auto force = body.Get("force");
	if (!force.Is(JsonKind::MISSING) && !force.Is(JsonKind::BOOLEAN)) {
		throw ApiError("E_BAD_REQUEST", 422, "force must be true or false");
	}
	run.force = force.Bool();
	const auto params = body.Get("params");
	if (!params.Is(JsonKind::MISSING) && !params.Is(JsonKind::OBJECT)) {
		throw ApiError("E_BAD_REQUEST", 422, "params must be an object");
	}
	for (const auto &name : params.Keys()) {
		run.params[name] = ToParamInput(name, params.Get(name));
	}
	if (!FindDefinition(connection, query_id, hash, run.workbook_id, run.definition)) {
		throw ApiError("E_NOT_FOUND", 404, "No query " + query_id + " with hash " + hash);
	}

	const auto started = RunEngine::Get().Start(*connection.context->db, run);
	SendJson(response, 202, RunJson(FindRun(connection, started.run_id)).Serialize());
}

void GetRunRoute(const httplib::Request &request, httplib::Response &response, duckdb::Connection &connection) {
	SendJson(response, 200, RunJson(FindRun(connection, request.matches[1].str())).Serialize());
}

//! A finished run is left as it is.
void CancelRunRoute(const httplib::Request &request, httplib::Response &response, duckdb::Connection &connection) {
	const auto run_id = request.matches[1].str();
	FindRun(connection, run_id);
	auto &engine = RunEngine::Get();
	if (engine.Cancel(run_id)) {
		engine.WaitFor(run_id, CANCEL_WAIT);
	}
	SendJson(response, 200, RunJson(FindRun(connection, run_id)).Serialize());
}

void RunRowsRoute(const httplib::Request &request, httplib::Response &response, duckdb::Connection &connection) {
	const auto run = FindRun(connection, request.matches[1].str());
	const auto status = run[STATUS].ToString();
	if (status != "succeeded") {
		throw ApiError("E_RUN_NOT_FINISHED", 409,
		               status == "running" || status == "queued"
		                   ? "The run is still going; its rows are available once it succeeds"
		                   : "The run did not succeed (" + status + "), so it has no rows");
	}
	TouchInstance(connection, run[INSTANCE_KEY].ToString());
	SendRowsPage(request, response, connection, run[RESULT_TABLE].ToString(), run[DEFINITION_HASH].ToString(),
	             run[TRUNCATED].GetValue<bool>());
}

void ListRunsRoute(const httplib::Request &request, httplib::Response &response, duckdb::Connection &connection) {
	const auto limit = IntegerParam(request, "limit", DEFAULT_RUNS_LISTED, 1, MAX_RUNS_LISTED);
	const auto rows = QueryRows(connection,
	                            std::string(RUN_COLUMNS) +
	                                "WHERE ($1 = '' OR r.query_id = lower($1)) AND ($2 = '' OR r.workbook_id = $2) "
	                                "ORDER BY r.started_at DESC, r.run_id LIMIT $3",
	                            {duckdb::Value(UrlParam(request, "query")),
	                             duckdb::Value(UrlParam(request, "workbook")), duckdb::Value::BIGINT(limit)});
	JsonObject body;
	auto runs = body.SetArray("runs");
	for (const auto &row : rows) {
		runs.AddObject(RunJson(row));
	}
	SendJson(response, 200, body.Serialize());
}

// --- /v1/preview -----------------------------------------------------------------

//! limit: an integer from 1 to PREVIEW_MAX_ROWS; absent is PREVIEW_MAX_ROWS.
int64_t PreviewLimit(const JsonValue &body) {
	const auto value = body.Get("limit");
	if (value.Is(JsonKind::MISSING)) {
		return PREVIEW_MAX_ROWS;
	}
	const auto text = value.Is(JsonKind::NUMBER) ? value.NumberText() : "";
	const bool digits =
	    !text.empty() && text.size() <= 3 && std::all_of(text.begin(), text.end(), [](unsigned char c) {
		    return std::isdigit(c);
	    });
	const auto limit = digits ? std::stoll(text) : 0;
	if (limit < 1 || limit > PREVIEW_MAX_ROWS) {
		throw ApiError("E_BAD_REQUEST", 422, "limit must be an integer from 1 to " + std::to_string(PREVIEW_MAX_ROWS));
	}
	return limit;
}

//! The text an author is writing needs no trust; a registered query does,
//! as a run would (SPEC.md §8.3).
void PreviewRoute(const httplib::Request &request, httplib::Response &response, duckdb::Connection &connection) {
	const auto document = ParseBody(request);
	const auto body = document.Root();
	const auto text = body.Get("text");
	const auto query = body.Get("query");
	const bool has_text = !text.Is(JsonKind::MISSING) && !text.Is(JsonKind::NULL_VALUE);
	const bool has_query = !query.Is(JsonKind::MISSING) && !query.Is(JsonKind::NULL_VALUE);
	if (has_text == has_query) {
		throw ApiError("E_BAD_REQUEST", 422, "Pass exactly one of text and query");
	}

	PreviewRequest preview;
	if (has_text) {
		if (!text.Is(JsonKind::STRING)) {
			throw ApiError("E_BAD_REQUEST", 422, "text must be a string");
		}
		preview.text = text.String();
	} else {
		if (!query.Is(JsonKind::OBJECT)) {
			throw ApiError("E_BAD_REQUEST", 422, "query must be an object {id, hash}");
		}
		const auto query_id = RequiredString(query, "id");
		const auto hash = RequiredString(query, "hash");
		// Any source: a hash is one text, and trust is the hash's.
		QueryDefinition definition;
		if (!FindDefinitionByHash(connection, hash, definition) || Lower(definition.query_id) != Lower(query_id)) {
			throw ApiError("E_NOT_FOUND", 404, "No query " + query_id + " with hash " + hash);
		}
		if (!IsDefinitionTrusted(connection, definition)) {
			throw ApiError("E_NOT_TRUSTED", 403,
			               "Query " + definition.query_id +
			                   " is not trusted. Confirm the workbook's queries before running them");
		}
		preview.text = definition.text;
	}
	preview.connection = OptionalString(body, "connection");
	const auto params = body.Get("params");
	if (!params.Is(JsonKind::MISSING) && !params.Is(JsonKind::OBJECT)) {
		throw ApiError("E_BAD_REQUEST", 422, "params must be an object");
	}
	for (const auto &name : params.Keys()) {
		preview.params[name] = ToParamInput(name, params.Get(name));
	}
	preview.limit = PreviewLimit(body);

	const auto result = RunPreview(*connection.context->db, preview);
	JsonObject out;
	auto columns = out.SetArray("columns");
	for (const auto &column : result.columns) {
		JsonObject info;
		info.Set("name", column.name).Set("header", column.header).Set("type", column.type.ToString());
		columns.AddObject(info);
	}
	auto rows = out.SetArray("rows");
	for (const auto &values : result.rows) {
		auto row = rows.AddArray();
		for (size_t i = 0; i < values.size(); i++) {
			AddCell(row, result.columns[i].type, values[i]);
		}
	}
	out.Set("elapsed_ms", result.elapsed_ms).Set("truncated", result.truncated);
	if (result.oracle) {
		out.Set("rendered_sql", result.rendered_sql);
	}
	auto warnings = out.SetArray("warnings");
	for (const auto &warning : result.warnings) {
		warnings.AddObject(ErrorJson(warning.code, warning.message));
	}
	SendJson(response, 200, out.Serialize());
}

// --- /v1/datasets ----------------------------------------------------------------

const char *const DATASET_COLUMNS = "SELECT instance_key, query_id, definition_hash, connection_name, params_json, "
                                    "status, fetched_at, row_count, truncated, last_error_code, last_error, "
                                    "current_table, prev_table FROM meta.dataset_instances ";

enum DatasetColumn {
	D_INSTANCE_KEY,
	D_QUERY_ID,
	D_DEFINITION_HASH,
	D_CONNECTION_NAME,
	D_PARAMS_JSON,
	D_STATUS,
	D_FETCHED_AT,
	D_ROW_COUNT,
	D_TRUNCATED,
	D_LAST_ERROR_CODE,
	D_LAST_ERROR,
	D_CURRENT_TABLE,
	D_PREV_TABLE
};

Row FindDataset(duckdb::Connection &connection, const std::string &instance_key) {
	auto rows =
	    QueryRows(connection, std::string(DATASET_COLUMNS) + "WHERE instance_key = $1", {duckdb::Value(instance_key)});
	if (rows.empty()) {
		throw ApiError("E_NOT_FOUND", 404, "No dataset " + instance_key);
	}
	return std::move(rows[0]);
}

void ListDatasetsRoute(const httplib::Request &, httplib::Response &response, duckdb::Connection &connection) {
	JsonObject body;
	auto datasets = body.SetArray("datasets");
	for (const auto &row : QueryRows(connection, std::string(DATASET_COLUMNS) + "ORDER BY query_id, instance_key")) {
		JsonObject dataset;
		dataset.Set("instance_key", row[D_INSTANCE_KEY].ToString())
		    .Set("query", QueryRef(row[D_QUERY_ID].ToString(), row[D_DEFINITION_HASH].ToString()))
		    .Set("connection", row[D_CONNECTION_NAME].ToString())
		    .Set("params", ParamsJson(row[D_PARAMS_JSON].ToString()))
		    .Set("status", row[D_STATUS].ToString());
		if (!row[D_FETCHED_AT].IsNull()) {
			dataset.Set("fetched_at", IsoTime(row[D_FETCHED_AT]));
		}
		if (!row[D_ROW_COUNT].IsNull()) {
			dataset.Set("row_count", row[D_ROW_COUNT].GetValue<int64_t>());
		}
		if (!row[D_TRUNCATED].IsNull()) {
			dataset.Set("truncated", row[D_TRUNCATED].GetValue<bool>());
		}
		if (!row[D_LAST_ERROR_CODE].IsNull()) {
			dataset.Set("last_error", ErrorJson(row[D_LAST_ERROR_CODE].ToString(),
			                                    row[D_LAST_ERROR].IsNull() ? "" : row[D_LAST_ERROR].ToString()));
		}
		datasets.AddObject(dataset);
	}
	SendJson(response, 200, body.Serialize());
}

void DatasetRowsRoute(const httplib::Request &request, httplib::Response &response, duckdb::Connection &connection) {
	const auto dataset = FindDataset(connection, request.matches[1].str());
	if (dataset[D_STATUS].ToString() != "ready") {
		throw ApiError("E_DEPENDENCY_MISSING", 409, "The dataset has no data yet: no run of it has succeeded");
	}
	TouchInstance(connection, dataset[D_INSTANCE_KEY].ToString());
	const bool truncated = !dataset[D_TRUNCATED].IsNull() && dataset[D_TRUNCATED].GetValue<bool>();
	SendRowsPage(request, response, connection, dataset[D_CURRENT_TABLE].ToString(),
	             dataset[D_DEFINITION_HASH].ToString(), truncated);
}

//! Drops the instance's tables and its row. Its runs stay in the history; their
//! rows answer E_GONE from now on.
void DeleteDatasetRoute(const httplib::Request &request, httplib::Response &response, duckdb::Connection &connection) {
	const auto instance_key = request.matches[1].str();
	FindDataset(connection, instance_key);
	// Checked before the metadata lock is taken: a run starts under the
	// engine's lock and then takes the metadata lock, and taking them the other
	// way round here could deadlock.
	if (RunEngine::Get().InstanceBusy(instance_key)) {
		throw ApiError("E_CONFLICT", 409, "The dataset is being refreshed; cancel the run or wait for it to finish");
	}
	std::lock_guard<std::mutex> guard(MetaWriteLock());
	InTransaction(connection, [&]() {
		const auto dataset = FindDataset(connection, instance_key);
		for (const auto column : {D_CURRENT_TABLE, D_PREV_TABLE}) {
			if (!dataset[column].IsNull()) {
				Execute(connection, "DROP TABLE IF EXISTS ds." + QuoteIdentifier(dataset[column].ToString()));
			}
		}
		QueryRows(connection, "DELETE FROM meta.dataset_instances WHERE instance_key = $1",
		          {duckdb::Value(instance_key)});
		// What formulas read changed: their cached results are stale (§10.3).
		Execute(connection, "UPDATE meta.state SET value = CAST(CAST(value AS BIGINT) + 1 AS VARCHAR) "
		                    "WHERE key = 'data_version'");
	});
	response.status = 204;
}

} // namespace

void RegisterRunRoutes(httplib::Server &server, const RouteContext &context) {
	server.Post("/v1/runs", WithConnection(context, StartRunRoute));
	server.Get("/v1/runs", WithConnection(context, ListRunsRoute));
	server.Get(R"(/v1/runs/([^/]+))", WithConnection(context, GetRunRoute));
	server.Post(R"(/v1/runs/([^/]+)/cancel)", WithConnection(context, CancelRunRoute));
	server.Get(R"(/v1/runs/([^/]+)/rows)", WithConnection(context, RunRowsRoute));
	server.Post("/v1/preview", WithConnection(context, PreviewRoute));
	server.Get("/v1/datasets", WithConnection(context, ListDatasetsRoute));
	server.Get(R"(/v1/datasets/([^/]+)/rows)", WithConnection(context, DatasetRowsRoute));
	server.Delete(R"(/v1/datasets/([^/]+))", WithConnection(context, DeleteDatasetRoute));
}

} // namespace ofexcel
