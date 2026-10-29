#pragma once

// A query file: the `-- @…` header and the body (CONTRACTS.md §2). Pure: no
// DuckDB. The body is returned as text; checking it is the renderer's (§4)
// and the Local SQL validator's (§7) job.

#include "ofexcel/contract_types.hpp"
#include "ofexcel/hashing.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace ofexcel {

enum class Engine { ORACLE, LOCAL };

struct QueryParam {
	std::string name;
	ParamType type;
	bool has_default = false;
	//! In the type's canonical form (§3.1); a list for list<T>.
	CanonicalValue default_value;
	std::string description;
	int line = 0;
};

struct QueryColumn {
	//! As written; compared with result columns without regard to case.
	std::string name;
	ColumnType type;
	std::string header;
	int line = 0;
};

struct DependencyArgument {
	enum class Kind { PARAM, STRING, NUMBER, DATE, BOOLEAN };
	std::string name;
	Kind kind = Kind::STRING;
	//! PARAM: the parameter's name, without "$". STRING: the text, quotes
	//! undoubled. NUMBER: as written. DATE: YYYY-MM-DD. BOOLEAN: true/false.
	std::string value;
};

struct Dependency {
	enum class Kind { QUERY, IMPORT };
	Kind kind = Kind::QUERY;
	//! QUERY: the query id. IMPORT: the name after "xl_import.".
	std::string name;
	std::vector<DependencyArgument> arguments;
	int line = 0;
};

struct QueryHeader {
	std::string id;
	Engine engine = Engine::ORACLE;
	int32_t version = 0;
	std::string title;
	//! @description lines joined with "\n".
	std::string description;
	//! "" if not given.
	std::string connection;
	//! In declaration order: the order of a formula's positional arguments (§10).
	std::vector<QueryParam> params;
	std::vector<QueryColumn> columns;
	std::vector<Dependency> depends;
	//! 0 if not given.
	int64_t max_rows = 0;

	//! Everything after the header, starting at line `body_line`, with line
	//! ends already "\n" (§1.1).
	std::string body;
	int body_line = 0;
};

struct QueryParseError {
	//! 1-based; 0 for the file as a whole (a missing directive).
	int line = 0;
	//! E_QUERY_PARSE or E_UNSUPPORTED (CONTRACTS.md §14).
	std::string code;
	//! Russian, for the author.
	std::string message;

	//! "строка 3: …", or the message alone for line 0.
	std::string ToString() const;
};

struct ParsedQueryFile {
	QueryHeader header;
	//! Every problem found, in line order; parsing goes on past the first one.
	std::vector<QueryParseError> errors;

	bool Ok() const {
		return errors.empty();
	}
};

//! Parses a query file. `text` is the file as read: the BOM and line ends are
//! normalized first, as for definition_hash, so line numbers match the editor.
ParsedQueryFile ParseQueryFile(const std::string &text);

//! ^[a-z][a-z0-9_]{0,62}$ (CONTRACTS.md §1).
bool IsIdentifier(const std::string &text);

} // namespace ofexcel
