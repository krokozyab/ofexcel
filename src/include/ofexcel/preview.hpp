#pragma once

// Preview (CONTRACTS.md §8.3 POST /v1/preview; TASKS.md C6): the first rows of
// a query with their contract types, read the way a run reads them (§5.1, §5.3)
// but held in memory. Nothing is stored: no run, no dataset instance, no schema
// in meta.query_schemas.

#include "duckdb.hpp"
#include "ofexcel/contract_types.hpp"
#include "ofexcel/param_values.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace ofexcel {

//! Rows a Preview returns at most (§5.4), and the page it asks ofquack for (§5.1).
constexpr int64_t PREVIEW_MAX_ROWS = 200;

struct PreviewRequest {
	//! The query file. The route takes it from `text`, or from the registered
	//! definition `query` names after checking trust.
	std::string text;
	//! "" takes @connection; for a Local query, the connection of its Oracle
	//! dependencies.
	std::string connection;
	std::map<std::string, ParamInput> params;
	//! 1..PREVIEW_MAX_ROWS.
	int64_t limit = PREVIEW_MAX_ROWS;
};

struct PreviewColumn {
	std::string name;
	//! From @column, else the name.
	std::string header;
	ColumnType type;
};

struct PreviewWarning {
	std::string code;
	std::string message;
};

struct PreviewResult {
	std::vector<PreviewColumn> columns;
	//! Each value as the column's storage type holds it (§3.2), NULL as NULL:
	//! what /rows would read from a stored version.
	std::vector<std::vector<duckdb::Value>> rows;
	int64_t elapsed_ms = 0;
	bool truncated = false;
	//! Oracle only: the SQL sent to Fusion, literals substituted (§4).
	bool oracle = false;
	std::string rendered_sql;
	std::vector<PreviewWarning> warnings;
};

//! Reads at most `limit` rows of the query, and one more to know whether
//! there are more. Throws ApiError: E_QUERY_PARSE, E_PARAM_*, E_SQL_*,
//! E_VALIDATION, E_BAD_REQUEST and E_NOT_FOUND for the connection, and
//! E_DEPENDENCY_MISSING (409) for a Local query whose dependency has no data
//! (Preview runs no dependency); then what the query raises, mapped by §14.1,
//! E_TYPE_DECLARED_MISMATCH (422) and E_TIMEOUT after ofexcel_run_timeout.
PreviewResult RunPreview(duckdb::DatabaseInstance &db, const PreviewRequest &request);

} // namespace ofexcel
