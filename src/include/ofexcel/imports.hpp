#pragma once

// Importing a table from the sheet (CONTRACTS.md §8.3 /v1/imports, §12;
// TASKS.md C7): begun with its columns, filled in batches, then committed as
// xl_import.<name> in one transaction. Until then the rows wait in a stage
// table, ds.stage__<import_id>, which the cleanup at start drops like a run's
// (§6.4); an import with no batch or commit for 10 minutes is dropped.

#include "duckdb.hpp"
#include "ofexcel/contract_types.hpp"
#include "ofexcel/param_values.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace ofexcel {

//! An import not used for this long is dropped (§8.3).
constexpr std::chrono::seconds IMPORT_EXPIRY = std::chrono::minutes(10);
//! Rows an import holds at most: a whole Excel sheet.
constexpr int64_t MAX_IMPORT_ROWS = 1048576;

struct ImportColumn {
	//! As the sheet's header has it; compared without regard to case.
	std::string name;
	//! A §3.2 type other than timestamptz.
	std::string type;
};

struct CommittedImport {
	std::string name;
	int64_t rows = 0;
};

//! The imports begun and not yet committed, in this process.
class ImportStore {
public:
	using Clock = std::function<std::chrono::steady_clock::time_point()>;

	static ImportStore &Get();

	//! POST /v1/imports: checks the request and creates the stage table.
	//! Returns the import_id. Throws ApiError E_BAD_REQUEST (422).
	std::string Begin(duckdb::Connection &connection, const std::string &name, const std::string &workbook_id,
	                  const std::string &source, const std::vector<ImportColumn> &columns);

	//! POST /v1/imports/{id}/rows: values encoded as §3.2 encodes them, an
	//! empty cell as null or "". All or nothing: a value that does not fit
	//! its column is E_BAD_REQUEST (422) naming the row and column, and no row
	//! of the batch is kept. An unknown or expired import is E_NOT_FOUND (404).
	void AddRows(duckdb::Connection &connection, const std::string &import_id,
	             const std::vector<std::vector<ParamInput>> &rows);

	//! POST /v1/imports/{id}/commit: replaces xl_import.<name> with the rows,
	//! records it in meta.imports and moves data_version, in one transaction.
	CommittedImport Commit(duckdb::Connection &connection, const std::string &import_id);

	//! Drops every import not used for IMPORT_EXPIRY. Every import route calls
	//! it first.
	void DropExpired(duckdb::Connection &connection);

	//! For tests: the clock expiry is measured by.
	void SetClock(Clock clock);

private:
	struct Pending {
		std::string name;
		std::string workbook_id;
		std::string source;
		std::vector<std::string> column_names;
		std::vector<ColumnType> column_types;
		std::string stage;
		int64_t rows = 0;
		std::chrono::steady_clock::time_point last_used;
	};

	ImportStore();
	Pending &Find(const std::string &import_id);

	//! Guards `pending` and `clock`, and serializes the batches and commits:
	//! an import's rows are appended one batch at a time.
	std::mutex mutex;
	std::map<std::string, Pending> pending;
	Clock clock;
};

} // namespace ofexcel
