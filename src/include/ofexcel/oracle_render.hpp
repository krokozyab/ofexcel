#pragma once

// Parameter substitution in Oracle SQL (CONTRACTS.md §4): a tokenizer that
// finds :name outside literals, quoted identifiers and comments, and literals
// rendered from canonical values. BIP's SQL report takes no bind variables,
// so this is the only way a value reaches Oracle; every rule has a vector in
// test/cpp/oracle_render_test.cpp. Pure: no DuckDB.

#include "ofexcel/param_values.hpp"
#include "ofexcel/query_file.hpp"

#include <map>
#include <string>
#include <vector>

namespace ofexcel {

//! Elements per sys.odci*list call (§4.2).
constexpr size_t ORACLE_LIST_CHUNK = 999;

struct RenderResult {
	bool ok = false;
	std::string rendered;
	//! Every declared parameter's canonical value, defaults applied: what
	//! canonical_params is built from (§1.2).
	std::map<std::string, CanonicalValue> values;
	//! Russian: declared parameters the text never uses.
	std::vector<std::string> warnings;

	//! !ok: a code from §14 (E_SQL_TOKENIZE, E_SQL_NOT_SELECT, E_PARAM_*) and a
	//! message for the user that names the parameter.
	std::string code;
	std::string message;
};

//! Renders `body` with `params` declared and `inputs` sent, keyed by
//! parameter name. Checks the text first, then the values, so an error in
//! the SQL is reported before a missing value.
RenderResult RenderOracle(const std::string &body, const std::vector<QueryParam> &params,
                          const std::map<std::string, ParamInput> &inputs);

//! The Oracle literal for one canonical value of `type` (§4.2). For list<T>,
//! the subquery; it belongs right after IN.
std::string OracleLiteral(const ParamType &type, const CanonicalValue &value);

} // namespace ofexcel
