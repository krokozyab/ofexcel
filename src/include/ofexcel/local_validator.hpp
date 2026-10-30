#pragma once

// Local SQL validation (CONTRACTS.md §7): rules V1–V9 over duckdb::Parser's
// tree. An additional defence, not a sandbox (SPEC.md §8.4): the daemon runs
// with enable_external_access = false regardless. Knows DuckDB's parser, so it
// lives in src/run/ rather than src/catalog/.

#include "ofexcel/query_file.hpp"

#include <string>
#include <vector>

namespace ofexcel {

//! What the query may refer to: its @depends and its @param.
struct LocalSqlScope {
	//! @depends query ids.
	std::vector<std::string> queries;
	//! @depends imports, the names after "xl_import.".
	std::vector<std::string> imports;
	//! @param names.
	std::vector<std::string> params;

	static LocalSqlScope FromHeader(const QueryHeader &header);
	//! From ofexcel_validate's arguments: each depends entry is "query_id",
	//! "query_id(…)" or "xl_import.name".
	static LocalSqlScope FromLists(const std::vector<std::string> &depends, const std::vector<std::string> &params);
};

struct LocalSqlVerdict {
	bool ok = false;
	//! E_VALIDATION for a broken rule; E_SQL_COMPILE if the text does not parse.
	std::string code;
	//! "V1"…"V8". V9 is how a rule reaches into subqueries and CTEs, so the
	//! rule reported is the one broken there.
	std::string rule;
	//! What broke it: a table, a function, a parameter.
	std::string fragment;
	//! English, for the author: rule, fragment and what to do.
	std::string message;
	//! $name references, lower case, unique, in the order the walk met them;
	//! on a violation, those met before it. Empty if the text does not parse.
	std::vector<std::string> params_in_text;
};

LocalSqlVerdict ValidateLocalSql(const std::string &sql, const LocalSqlScope &scope);

//! The parameters a Local query passes on to its @depends as $name, lower
//! case: used, though not in its SQL, so not W_PARAM_UNUSED.
std::vector<std::string> ParamsPassedOn(const QueryHeader &header);

} // namespace ofexcel
