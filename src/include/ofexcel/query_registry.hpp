#pragma once

// The query registry (CONTRACTS.md §6.2 meta.query_definitions, §8.3, §10.2;
// TASKS.md C1): definitions from project files, workbooks and the built-in
// examples, and resolving a query by name. Every change recomputes
// catalog_version in the same transaction.

#include "duckdb.hpp"
#include "ofexcel/query_file.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace ofexcel {

constexpr const char *SOURCE_PROJECT = "project";
constexpr const char *SOURCE_WORKBOOK = "workbook";
constexpr const char *SOURCE_EXAMPLE = "example";
//! source_ref of every example.
constexpr const char *EXAMPLES_REF = "builtin";

//! meta.trusted_definitions.origin (CONTRACTS.md §6.2).
constexpr const char *TRUST_LOCAL_EDITOR = "local_editor";
constexpr const char *TRUST_USER_CONSENT = "user_consent";
constexpr const char *TRUST_EXAMPLE = "example";

//! A query file larger than this is refused rather than read: a definition is
//! a header and one statement.
constexpr size_t MAX_QUERY_FILE_BYTES = 1024 * 1024;

//! One row of meta.query_definitions.
struct QueryDefinition {
	std::string definition_hash;
	std::string query_id;
	int32_t version = 0;
	std::string engine;      //!< oracle or local
	std::string source_kind; //!< SOURCE_PROJECT, SOURCE_WORKBOOK or SOURCE_EXAMPLE
	std::string source_ref;  //!< project name, workbook_id or EXAMPLES_REF
	std::string text;        //!< the file as given
	//! May run without asking (SPEC.md §8.3): its hash is in
	//! meta.trusted_definitions, or a project or the examples have the same text.
	bool trusted = false;
};

//! A query file by name, for registering a set of them.
struct QueryText {
	std::string name;
	std::string text;
};

//! One file ofexcel_reload() looked at.
struct ReloadEntry {
	std::string source;   //!< <project>/queries/<file>
	std::string query_id; //!< "" when the header has no usable @id
	bool ok = false;
	std::string message; //!< "" when ok
};

//! The examples built into ofexceld (TASKS.md E5); none yet.
const std::vector<QueryText> &BuiltinExamples();

//! header_json: the parsed header as canonical JSON (CONTRACTS.md §1).
std::string HeaderJson(const QueryHeader &header);

//! A project name a directory can have on every platform: not empty, at most
//! 100 bytes of UTF-8, no control characters or / \ : * ? " < > |, not
//! starting with "." and not ending with "." or a space.
bool IsProjectName(const std::string &name);

//! Reads every <projects_dir>/<project>/queries/*.sql and makes the project
//! definitions exactly what parsed. A missing projects_dir means no projects.
//! Returns one entry per file, in project and file name order.
std::vector<ReloadEntry> ReloadProjects(duckdb::Connection &connection, const std::string &projects_dir);

//! PUT /v1/queries/{id}: writes <projects_dir>/<project>/queries/<id>.sql and
//! rereads that project. Throws ApiError: E_BAD_REQUEST (422) for a bad id or
//! project name, E_QUERY_PARSE (422) for a header that does not parse or an
//! @id other than `query_id`.
QueryDefinition SaveProjectQuery(duckdb::Connection &connection, const std::string &projects_dir,
                                 const std::string &project, const std::string &query_id, const std::string &text);

//! PUT /v1/workbooks/{id}/queries: replaces the workbook's set. All or nothing:
//! any text that does not parse throws ApiError E_QUERY_PARSE (422) with every
//! query's errors in details, and the registry is left as it was. Returns the
//! definitions in the order of `texts`.
std::vector<QueryDefinition> RegisterWorkbook(duckdb::Connection &connection, const std::string &workbook_id,
                                              const std::vector<std::string> &texts);

//! Replaces the examples. Throws std::runtime_error if one does not parse: they
//! are built in, so that is a bug.
void RegisterExamples(duckdb::Connection &connection, const std::vector<QueryText> &examples);

//! GET /v1/queries: the workbook's definitions (if `workbook_id` is not ""),
//! then projects, then examples; by id, newest version first.
std::vector<QueryDefinition> ListDefinitions(duckdb::Connection &connection, const std::string &workbook_id);

//! Which definitions FindQuery chooses from.
struct QueryScope {
	//! §10.2: the workbook's queries come first. "" for none.
	std::string workbook_id;
	//! Only this source kind, and source_ref if that is not "" too.
	std::string source_kind;
	std::string source_ref;
};

//! Resolves `query_id`, without regard to case, as §10.2 does: the workbook,
//! then projects, then examples; within that, the highest @version, and on a
//! tie the first source_ref, then the first hash, so the choice never depends
//! on row order. False if there is none.
bool FindQuery(duckdb::Connection &connection, const std::string &query_id, const QueryScope &scope,
               QueryDefinition &found);

//! A registered definition with this hash, the first in ListDefinitions'
//! order; false if the text is not registered anywhere.
bool FindDefinitionByHash(duckdb::Connection &connection, const std::string &definition_hash, QueryDefinition &found);

//! meta.query_definitions for ofexcel_queries(), in ListDefinitions' order
//! with every workbook included.
std::vector<QueryDefinition> AllDefinitions(duckdb::Connection &connection);

//! POST /v1/trust: trusts `hashes` with origin user_consent. All or nothing:
//! a hash that is not among the workbook's queries throws ApiError
//! E_BAD_REQUEST (422) and trusts none. A hash trusted already keeps its
//! origin. Returns the hashes, each once, in the order given.
std::vector<std::string> TrustWorkbookQueries(duckdb::Connection &connection, const std::string &workbook_id,
                                              const std::vector<std::string> &hashes);

//! Whether `definition` may run (SPEC.md §8.3), asked of the database now
//! rather than taken from `definition.trusted`: by hash, like `trusted`.
bool IsDefinitionTrusted(duckdb::Connection &connection, const QueryDefinition &definition);

//! A QueryRef {id, hash}: the definition with this hash and id (without regard
//! to case) among the workbook's queries, projects and examples, in that order.
//! False if there is none.
bool FindDefinition(duckdb::Connection &connection, const std::string &query_id, const std::string &definition_hash,
                    const std::string &workbook_id, QueryDefinition &found);

//! catalog_version for the registry as it is: the first 16 hex digits of
//! SHA-256 over every (source_kind, source_ref, query_id, definition_hash),
//! sorted, or "" for an empty registry. The same files give the same version
//! after a restart.
std::string ComputeCatalogVersion(duckdb::Connection &connection);

} // namespace ofexcel
