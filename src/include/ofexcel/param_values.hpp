#pragma once

// A parameter value from the add-in, as JSON, brought to its canonical form
// (CONTRACTS.md §3.1). Pure: no DuckDB, no JSON library — the HTTP layer turns
// its parsed JSON into a ParamInput, numbers as their raw text, so that no
// digit is lost on the way.

#include "ofexcel/contract_types.hpp"
#include "ofexcel/hashing.hpp"

#include <string>
#include <vector>

namespace ofexcel {

constexpr size_t MAX_LIST_ELEMENTS = 100000;

struct ParamInput {
	enum class Kind { MISSING, NULL_VALUE, BOOLEAN, NUMBER, STRING, ARRAY };
	Kind kind = Kind::MISSING;
	bool boolean = false;
	//! NUMBER: the JSON number token as written ("204", "-1.5", "1e+21").
	//! STRING: the string.
	std::string text;
	//! ARRAY: the elements.
	std::vector<ParamInput> elements;

	static ParamInput Missing();
	static ParamInput Null();
	static ParamInput Boolean(bool value);
	static ParamInput Number(std::string json_number);
	static ParamInput String(std::string value);
	static ParamInput Array(std::vector<ParamInput> elements);
};

struct ParamResult {
	bool ok = false;
	CanonicalValue value;
	//! E_PARAM_MISSING, E_PARAM_TYPE or E_PARAM_TOO_LONG (CONTRACTS.md §14).
	std::string code;
	//! English, for the user; the caller prefixes the parameter's name.
	std::string message;
};

//! The canonical value of `input` for `type`. An empty input — missing, null
//! or "" — takes `default_value`, or is E_PARAM_MISSING when that is null.
//! For list<T>, [] is a value, not an empty input.
ParamResult NormalizeParam(const ParamType &type, const CanonicalValue *default_value, const ParamInput &input);

} // namespace ofexcel
