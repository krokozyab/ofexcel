#pragma once

// What every run shares, whatever its engine (CONTRACTS.md §5.4, §6.3, §6.4):
// the plan Start settles, the watchdog, the stage table a result streams into,
// and the new version's table and swap. The engines are oracle_run.cpp (§5.1)
// and local_run.cpp (§5.3); the engine that starts them is run_engine.cpp.

#include "duckdb.hpp"
#include "duckdb/main/query_result_stream.hpp"
#include "ofexcel/api_error.hpp"
#include "ofexcel/datasets.hpp"
#include "ofexcel/hashing.hpp"
#include "ofexcel/query_file.hpp"
#include "ofexcel/run_engine.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
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
	//! The connection reading the result, while it is.
	duckdb::Connection *reader = nullptr;
};

//! How often a cancelled or timed-out run's reader is interrupted again, and
//! how often a Local run waiting for its dependencies looks whether it should
//! stop. An interrupt that lands before DuckDB starts the query is cleared
//! when it does, so one is not enough.
constexpr auto INTERRUPT_REPEAT = std::chrono::milliseconds(100);

//! §5.1, exactly. The SQL and the secret are parameters, never spliced in.
constexpr const char *ORACLE_QUERY_SQL =
    "SELECT * FROM oracle_fusion_query($1, secret := $2, all_varchar := true, on_cast_error := 'error', "
    "fetch_size := $3)";

//! An integer column of a Local result holding a value this large is an id
//! (§3.4).
constexpr int64_t LOCAL_ID_FROM = 1000000000000000;

constexpr const char *WARNING_PARAM_UNUSED = "W_PARAM_UNUSED";
constexpr const char *WARNING_TYPE_WIDENED = "W_TYPE_WIDENED";
constexpr const char *WARNING_NUMBER_PRECISION = "W_NUMBER_PRECISION";

struct RunWarning {
	std::string code;
	std::string message;
};

std::string WarningsJson(const std::vector<RunWarning> &warnings);

std::string Lower(std::string text);

//! NULL for "".
duckdb::Value NullableString(const std::string &text);

duckdb::Value Now();

//! ofexcel_run_timeout, in seconds.
int64_t RunTimeoutSetting(duckdb::Connection &connection);

//! One @depends of a Local run, as Start resolved it (§5.3).
struct DependencyPlan {
	Dependency::Kind kind = Dependency::Kind::QUERY;
	//! The query id as registered, or the import's name.
	std::string name;
	//! QUERY: the run that refreshes it; parent_run_id is set when it starts.
	RunRequest request;
	std::string instance_key;
	//! QUERY: run it before the parent's SQL (`deps`).
	bool start = false;
};

//! What Start settles before the run begins. The run only reads it.
struct Plan {
	std::string run_id;
	std::string query_id;
	std::string definition_hash;
	std::string instance_key;
	//! The connection's name; LOCAL_CONNECTION_NAME for a Local query.
	std::string connection;
	Engine engine = Engine::ORACLE;
	//! ORACLE: the body with its literals substituted (§4).
	std::string rendered_sql;
	//! LOCAL: the body, run as it is with $name bound (§5.3).
	std::string body;
	std::vector<QueryParam> params;
	//! Every declared parameter's canonical value (§1.2).
	std::map<std::string, CanonicalValue> values;
	std::vector<DependencyPlan> depends;
	std::vector<QueryColumn> declared;
	int64_t max_rows = DEFAULT_MAX_ROWS;
	int64_t fetch_size = DEFAULT_FETCH_SIZE;
	int64_t timeout_seconds = DEFAULT_RUN_TIMEOUT_SECONDS;
	std::vector<RunWarning> warnings;
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

//! One run, on three connections of its own: `reader` runs the query and is
//! the one interrupted; `scanner` reads the stage table back; `writer` creates
//! tables, appends and writes meta.*. An open result belongs to its
//! connection, so a scan and an append cannot share one.
class RunWork {
public:
	RunWork(duckdb::DatabaseInstance &db, RunEngine::Active &run, const Plan &plan);
	virtual ~RunWork() = default;

	//! Runs to the end and records how it ended in meta.*.
	void Perform();

protected:
	//! Reads the result and commits the new version (Commit), or throws
	//! ApiError, Stopped, or whatever DuckDB threw.
	virtual void Fetch(Outcome &outcome) = 0;
	//! §14.1: the code of an error this engine's query raised.
	virtual ApiError MapError(const duckdb::ErrorData &error) const = 0;

	bool Stopping() const;
	void ThrowIfStopping() const;
	void SetReader(duckdb::Connection *connection);

	//! Submits the query on `reader` with `submit`, interruptible from then on,
	//! and streams its result into a new ds.<stage> with columns c0, c1, … of
	//! `stage_type` of each result type, keeping at most max_rows rows (§5.4).
	//! `names` and `types` get the result's. The appender casts a column whose
	//! stage type differs. Returns the query's error, if any, with its type
	//! intact for §14.1.
	duckdb::ErrorData StreamIntoStage(const std::function<duckdb::unique_ptr<duckdb::QueryResult>()> &submit,
	                                  const std::function<duckdb::LogicalType(const duckdb::LogicalType &)> &stage_type,
	                                  Outcome &outcome);

	//! Calls `visit` with every chunk of the stage table, in the order the rows
	//! were appended.
	void ScanStage(const std::function<void(duckdb::DataChunk &)> &visit);

	//! Creates the instance's next version, ds.<query>__<key>__v<N>, with
	//! columns stored as §3.2 stores their types.
	void CreateTarget(const std::vector<SchemaColumn> &schema, Outcome &outcome);

	//! §6.4: the new version becomes current, the one before it previous, and
	//! the one before that is dropped, in one transaction.
	void Commit(Outcome &outcome, const std::vector<SchemaColumn> &schema, bool write_schema);

	void DropTable(const std::string &table);

	duckdb::DatabaseInstance &db;
	RunEngine::Active &run;
	const Plan &plan;
	duckdb::Connection reader;
	duckdb::Connection scanner;
	duckdb::Connection writer;
	std::vector<RunWarning> warnings;
	//! The result's column names, as the query named them.
	std::vector<std::string> names;
	//! The result's column types, as the query returned them.
	std::vector<duckdb::LogicalType> types;
	std::string stage;

private:
	void StoppedOutcome(Outcome &outcome) const;
	void UpdateRun(const Outcome &outcome);
	void RecordFailure(Outcome &outcome);
	int64_t NextVersion();

	int64_t target_version = 0;
};

//! Interrupts the run's reader for as long as it is cancelled or past its
//! deadline, until the run is done.
void Watch(RunEngine::Active &run, int64_t timeout_seconds);

std::unique_ptr<RunWork> OracleWork(duckdb::DatabaseInstance &db, RunEngine::Active &run, const Plan &plan);
std::unique_ptr<RunWork> LocalWork(duckdb::DatabaseInstance &db, RunEngine::Active &run, const Plan &plan);

// --- Local SQL, shared by a Local run and Preview (local_run.cpp) -------------

//! §14.1 for Local SQL.
ApiError LocalSqlError(const duckdb::ErrorData &error);

bool IsIntegerType(const duckdb::LogicalType &type);

//! A Local result column's type before it is stored: a type §3.4 maps, close
//! enough to keep its values, else VARCHAR. Integers stay as they are: their
//! values decide between integer and id.
duckdb::LogicalType LocalStageType(const duckdb::LogicalType &type);

//! plan.values for the $name the prepared body uses, typed as §5.3 binds them.
duckdb::identifier_map_t<duckdb::BoundParameterData> BindLocalParams(const duckdb::PreparedStatement &prepared,
                                                                     const Plan &plan);

//! §5.3: each dependency under its short name, as a temporary view on
//! `reader`; they go with the connection. `meta` reads meta.*. A dependency
//! without data is E_DEPENDENCY_MISSING (409).
void CreateDependencyViews(duckdb::Connection &reader, duckdb::Connection &meta,
                           const std::vector<DependencyPlan> &depends);

//! §3.3 item 1 for a Local value in a column declared with @column: the
//! text to store, or E_TYPE_DECLARED_MISMATCH naming the 1-based `row`. ''
//! stays '' (DuckDB tells it from NULL), so only a string holds it.
duckdb::Value DeclaredLocalValue(const std::string &name, const QueryColumn &column, const duckdb::Value &value,
                                 int64_t row);

//! Start's part for a Local query (§5.3): validates the SQL (§7), normalizes
//! the parameters and resolves every @depends to the instance it reads. Throws
//! ApiError for a request that cannot run; E_DEPENDENCY_MISSING (409) records
//! the missing instances in meta.missing for the workbook first.
void PlanLocalRun(duckdb::Connection &connection, const QueryHeader &header, const RunRequest &request, Plan &plan);

} // namespace ofexcel
