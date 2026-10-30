#include "ofexcel/run_engine.hpp"

#include "duckdb/common/types/uuid.hpp"
#include "ofexcel/api_error.hpp"
#include "ofexcel/connections.hpp"
#include "ofexcel/datasets.hpp"
#include "ofexcel/db_util.hpp"
#include "ofexcel/hashing.hpp"
#include "ofexcel/oracle_render.hpp"
#include "ofexcel/query_file.hpp"
#include "run_work.hpp"

#include <iostream>
#include <thread>

namespace ofexcel {

namespace {

duckdb::Value Environment(duckdb::Connection &connection, const std::string &name) {
	const auto rows =
	    QueryRows(connection, "SELECT environment FROM meta.connections WHERE name = $1", {duckdb::Value(name)});
	return rows.empty() ? duckdb::Value(duckdb::LogicalType::VARCHAR) : rows[0][0];
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
		               "Query " + definition.query_id + " cannot be parsed: " + parsed.errors[0].ToString());
	}
	const auto &header = parsed.header;
	duckdb::Connection connection(db);
	// Before anything else about the run: an untrusted text is not rendered, and
	// no run or instance is recorded for it (SPEC.md §8.3). The hash is the
	// text's own, whatever the caller says it is.
	auto checked = definition;
	checked.definition_hash = DefinitionHash(definition.text);
	if (!IsDefinitionTrusted(connection, checked)) {
		throw ApiError("E_NOT_TRUSTED", 403,
		               "Query " + header.id + " is not trusted. Confirm the workbook's queries before running them");
	}
	auto plan = std::make_shared<Plan>();
	plan->query_id = header.id;
	plan->definition_hash = checked.definition_hash;
	plan->declared = header.columns;
	plan->max_rows = header.max_rows > 0 ? header.max_rows : DEFAULT_MAX_ROWS;
	plan->timeout_seconds = RunTimeoutSetting(connection);
	auto environment = duckdb::Value(duckdb::LogicalType::VARCHAR);
	auto user = duckdb::Value(duckdb::LogicalType::VARCHAR);
	if (header.engine == Engine::LOCAL) {
		PlanLocalRun(connection, header, request, *plan);
	} else {
		plan->connection = request.connection.empty() ? header.connection : request.connection;
		if (plan->connection.empty()) {
			throw ApiError("E_BAD_REQUEST", 422,
			               "No connection given: pass connection or declare @connection in query " + header.id);
		}
		if (!FusionSecretExists(connection, plan->connection)) {
			throw ApiError("E_NOT_FOUND", 404, "No connection " + plan->connection);
		}
		const auto rendered = RenderOracle(header.body, header.params, request.params);
		if (!rendered.ok) {
			throw ApiError(rendered.code, 422, rendered.message);
		}
		plan->rendered_sql = rendered.rendered;
		plan->values = rendered.values;
		for (const auto &message : rendered.warnings) {
			plan->warnings.push_back({WARNING_PARAM_UNUSED, message});
		}
		environment = Environment(connection, plan->connection);
		user = FusionUser(connection, plan->connection);
	}
	const auto params_json = CanonicalParams(plan->values);
	plan->instance_key = InstanceKey(header.id, plan->definition_hash, plan->connection, params_json);
	const auto &connection_name = plan->connection;

	std::lock_guard<std::mutex> lock(mutex);
	const auto going = by_instance.find(plan->instance_key);
	if (going != by_instance.end()) {
		if (request.force) {
			throw ApiError("E_CONFLICT", 409,
			               "This dataset is already being refreshed (run " + going->second +
			                   "), wait for it to finish");
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
				auto work = plan->engine == Engine::LOCAL ? LocalWork(*shared_db, *active, *plan)
				                                          : OracleWork(*shared_db, *active, *plan);
				work->Perform();
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

bool RunEngine::WaitFor(const std::string &run_id, std::chrono::milliseconds timeout) {
	std::unique_lock<std::mutex> lock(mutex);
	return idle.wait_for(lock, timeout, [&]() { return by_run.find(run_id) == by_run.end(); });
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

bool RunEngine::InstanceBusy(const std::string &instance_key) {
	std::lock_guard<std::mutex> lock(mutex);
	return by_instance.find(instance_key) != by_instance.end();
}

size_t RunEngine::ActiveRuns() {
	std::lock_guard<std::mutex> lock(mutex);
	return by_run.size();
}

} // namespace ofexcel
