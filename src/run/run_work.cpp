#include "run_work.hpp"

#include "duckdb/main/appender.hpp"
#include "ofexcel/db_util.hpp"

#include <algorithm>
#include <cctype>
#include <iostream>

namespace ofexcel {

std::string WarningsJson(const std::vector<RunWarning> &warnings) {
	auto array = CanonicalJson::Array();
	for (const auto &warning : warnings) {
		auto item = CanonicalJson::Object();
		item.Set("code", CanonicalJson::String(warning.code)).Set("message", CanonicalJson::String(warning.message));
		array.Add(std::move(item));
	}
	return array.Serialize();
}

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

RunWork::RunWork(duckdb::DatabaseInstance &db_p, RunEngine::Active &run_p, const Plan &plan_p)
    : db(db_p), run(run_p), plan(plan_p), reader(db_p), scanner(db_p), writer(db_p) {
	warnings = plan.warnings;
}

void RunWork::Perform() {
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
			const auto mapped = MapError(error);
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

bool RunWork::Stopping() const {
	return run.cancel_requested || run.timed_out;
}

void RunWork::ThrowIfStopping() const {
	if (Stopping()) {
		throw Stopped();
	}
}

void RunWork::StoppedOutcome(Outcome &outcome) const {
	if (run.timed_out) {
		outcome.status = "failed";
		outcome.error_code = "E_TIMEOUT";
		outcome.error_message = "The run was stopped: it took longer than " + std::to_string(plan.timeout_seconds) +
		                        " s (ofexcel_run_timeout)";
	} else {
		outcome.status = "cancelled";
		outcome.error_code = "E_CANCELLED";
		outcome.error_message = "The run was cancelled";
	}
}

void RunWork::SetReader(duckdb::Connection *connection) {
	std::lock_guard<std::mutex> lock(run.mutex);
	run.reader = connection;
}

duckdb::ErrorData
RunWork::StreamIntoStage(const std::function<duckdb::unique_ptr<duckdb::QueryResult>()> &submit,
                         const std::function<duckdb::LogicalType(const duckdb::LogicalType &)> &stage_type,
                         Outcome &outcome) {
	SetReader(&reader);
	// Cleared on every way out, so the watchdog never interrupts a connection
	// that is gone.
	struct ClearReader {
		RunWork &self;
		~ClearReader() {
			self.SetReader(nullptr);
		}
	} clear_reader {*this};

	auto handle = submit();
	if (handle->HasError()) {
		return handle->GetErrorObject();
	}
	duckdb::QueryResultStream<> stream(std::move(handle));
	for (const auto &name : stream.GetNames()) {
		names.push_back(name.GetIdentifierName());
	}
	for (const auto &type : stream.GetTypes()) {
		types.push_back(type);
	}

	std::string columns;
	for (size_t i = 0; i < types.size(); i++) {
		columns += (i ? ", c" : "c") + std::to_string(i) + " " + stage_type(types[i]).ToString();
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
		// Row max_rows + 1 is what tells a truncated result from one that fits
		// exactly (§5.4).
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
		// The rest is not needed: stop the query between chunks, or in one.
		reader.Interrupt();
		stream.Close();
	} else if (!error.HasError() && stream.HasError()) {
		error = stream.GetErrorObject();
	}
	appender.Close();
	outcome.rows = kept;
	return error;
}

void RunWork::ScanStage(const std::function<void(duckdb::DataChunk &)> &visit) {
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

int64_t RunWork::NextVersion() {
	const auto rows = QueryRows(writer, "SELECT current_version FROM meta.dataset_instances WHERE instance_key = $1",
	                            {duckdb::Value(plan.instance_key)});
	return (rows.empty() ? 0 : rows[0][0].GetValue<int64_t>()) + 1;
}

void RunWork::CreateTarget(const std::vector<SchemaColumn> &schema, Outcome &outcome) {
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

void RunWork::UpdateRun(const Outcome &outcome) {
	QueryRows(writer,
	          "UPDATE meta.runs SET status = $2, finished_at = $3, rows_fetched = $4, truncated = $5, "
	          "result_table = $6, warnings_json = $7, error_code = $8, error_message = $9 WHERE run_id = $1",
	          {duckdb::Value(plan.run_id), duckdb::Value(outcome.status), Now(), duckdb::Value::BIGINT(outcome.rows),
	           duckdb::Value::BOOLEAN(outcome.truncated), NullableString(outcome.result_table),
	           duckdb::Value(WarningsJson(warnings)), NullableString(outcome.error_code),
	           NullableString(outcome.error_message)});
}

void RunWork::Commit(Outcome &outcome, const std::vector<SchemaColumn> &schema, bool write_schema) {
	outcome.status = "succeeded";
	std::lock_guard<std::mutex> guard(MetaWriteLock());
	InTransaction(writer, [&]() {
		const auto previous = QueryRows(writer, "SELECT prev_table FROM meta.dataset_instances WHERE instance_key = $1",
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
		// The data is there now: no workbook misses it any more (§8.3 /v1/missing).
		QueryRows(writer, "DELETE FROM meta.missing WHERE instance_key = $1", {duckdb::Value(plan.instance_key)});
		if (write_schema) {
			WriteQuerySchema(writer, plan.definition_hash, schema);
		}
		UpdateRun(outcome);
	});
}

//! A failed or cancelled run: its own table goes, the instance keeps its
//! current version and learns why (§6.4).
void RunWork::RecordFailure(Outcome &outcome) {
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

void RunWork::DropTable(const std::string &table) {
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

} // namespace ofexcel
