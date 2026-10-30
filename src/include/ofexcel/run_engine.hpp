#pragma once

// Oracle runs (CONTRACTS.md §5.1, §5.2, §5.4, §6.3, §6.4, §14.1; TASKS.md C3):
// from the rendered SQL to a new version of the dataset instance. A run reads
// from ofquack on a connection of its own, the one cancellation and the
// timeout interrupt (DECISIONS.md R3), and writes on another.

#include "duckdb.hpp"
#include "ofexcel/param_values.hpp"
#include "ofexcel/query_registry.hpp"

#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace ofexcel {

//! Rows per BI Publisher request (§5.1).
constexpr int64_t DEFAULT_FETCH_SIZE = 10000;
//! Rows a run keeps without @max_rows (§5.4, DECISIONS.md R4).
constexpr int64_t DEFAULT_MAX_ROWS = 1000000;
//! ofexcel_run_timeout's default, in seconds (§5.4).
constexpr int64_t DEFAULT_RUN_TIMEOUT_SECONDS = 1800;

struct RunRequest {
	//! As the registry has it: the text is parsed again here, so a run never
	//! depends on a header parsed somewhere else.
	QueryDefinition definition;
	//! "" takes @connection.
	std::string connection;
	std::map<std::string, ParamInput> params;
	std::string workbook_id;
	std::string parent_run_id;
	//! A run of the same instance already going is E_CONFLICT instead of that
	//! run's id (§6.4).
	bool force = false;
};

struct RunStart {
	std::string run_id;
	//! The instance already had a run going, and this is its id.
	bool joined = false;
};

//! The runs going in this process. One per instance_key at a time (§6.4).
class RunEngine {
public:
	static RunEngine &Get();
	~RunEngine();

	//! Checks the request, renders the SQL, writes the run's row to meta.runs
	//! and the instance's to meta.dataset_instances, and starts reading in the
	//! background. Throws ApiError for a request that cannot run:
	//! E_QUERY_PARSE, E_UNSUPPORTED (a local query, TASKS.md C5), E_BAD_REQUEST
	//! (no connection), E_NOT_FOUND (no such connection), E_PARAM_* and
	//! E_SQL_* from rendering, E_CONFLICT with `force`.
	RunStart Start(duckdb::DatabaseInstance &db, const RunRequest &request);

	//! Returns once the run has finished and its row in meta.runs says how.
	//! Returns at once for a run that is not going.
	void Wait(const std::string &run_id);

	//! Interrupts the run; it ends as cancelled with E_CANCELLED, keeping the
	//! instance's current version. False if the run is not going.
	bool Cancel(const std::string &run_id);

	//! Rows read so far by a run that is going; -1 if it is not.
	int64_t RowsFetched(const std::string &run_id);

	size_t ActiveRuns();

	struct Active;

private:
	RunEngine() = default;
	void Finished(const std::string &run_id, const std::string &instance_key);

	std::mutex mutex;
	std::condition_variable idle;
	std::map<std::string, std::shared_ptr<Active>> by_run;
	//! instance_key → run_id.
	std::map<std::string, std::string> by_instance;
};

} // namespace ofexcel
