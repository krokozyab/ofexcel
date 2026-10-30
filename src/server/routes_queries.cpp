// The query catalog (CONTRACTS.md §8.3, TASKS.md C1): listing, reading,
// saving and validating query definitions, and a workbook's own set.

#include "routes.hpp"

#include "ofexcel/api_error.hpp"
#include "ofexcel/db_util.hpp"
#include "ofexcel/json_body.hpp"
#include "ofexcel/local_validator.hpp"
#include "ofexcel/oracle_render.hpp"
#include "ofexcel/query_file.hpp"
#include "ofexcel/query_registry.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace ofexcel {

namespace {

//! The code of the warning for a declared parameter the text never uses.
constexpr const char *WARNING_PARAM_UNUSED = "W_PARAM_UNUSED";

//! A list default as the author writes it: elements after "= ", by commas.
std::string DefaultText(const CanonicalValue &value) {
	if (!value.is_list) {
		return value.scalar;
	}
	std::string joined;
	for (size_t i = 0; i < value.list.size(); i++) {
		joined += (i ? ", " : "") + value.list[i];
	}
	return joined;
}

//! ColumnInfo[]: the @column declarations, or the schema the last run stored
//! for this definition (§3.3) when there are none.
void AddColumns(JsonObject &summary, const QueryHeader &header, duckdb::Connection &connection,
                const std::string &definition_hash) {
	auto columns = summary.SetArray("columns");
	if (!header.columns.empty()) {
		for (const auto &column : header.columns) {
			JsonObject item;
			item.Set("name", column.name)
			    .Set("header", column.header.empty() ? column.name : column.header)
			    .Set("type", column.type.ToString());
			columns.AddObject(item);
		}
		return;
	}
	for (const auto &row : QueryRows(connection,
	                                 "SELECT column_name, header, contract_type FROM meta.query_schemas "
	                                 "WHERE definition_hash = $1 ORDER BY ordinal",
	                                 {duckdb::Value(definition_hash)})) {
		JsonObject item;
		const auto name = row[0].ToString();
		item.Set("name", name).Set("header", row[1].IsNull() ? name : row[1].ToString()).Set("type", row[2].ToString());
		columns.AddObject(item);
	}
}

//! QuerySummary (§8.2) for a definition and its parsed header.
JsonObject Summary(const QueryDefinition &definition, const QueryHeader &header, duckdb::Connection &connection) {
	JsonObject summary;
	summary.Set("id", header.id)
	    .Set("hash", definition.definition_hash)
	    .Set("version", int64_t(header.version))
	    .Set("engine", header.engine == Engine::LOCAL ? "local" : "oracle");
	if (!header.title.empty()) {
		summary.Set("title", header.title);
	}
	if (!header.description.empty()) {
		summary.Set("description", header.description);
	}
	auto params = summary.SetArray("params");
	for (const auto &param : header.params) {
		JsonObject item;
		item.Set("name", param.name).Set("type", param.type.ToString());
		if (param.has_default) {
			item.Set("default", DefaultText(param.default_value));
		}
		if (!param.description.empty()) {
			item.Set("description", param.description);
		}
		params.AddObject(item);
	}
	AddColumns(summary, header, connection, definition.definition_hash);
	JsonObject source;
	source.Set("kind", definition.source_kind).Set("ref", definition.source_ref);
	summary.Set("source", source).Set("trusted", definition.trusted);
	return summary;
}

//! The summary of a registered definition: its text parsed again, which is
//! cheap and keeps one source of truth for the header.
JsonObject Summary(const QueryDefinition &definition, duckdb::Connection &connection) {
	const auto parsed = ParseQueryFile(definition.text);
	return Summary(definition, parsed.header, connection);
}

std::string UrlParam(const httplib::Request &request, const char *name) {
	return request.has_param(name) ? request.get_param_value(name) : "";
}

void SendSummaries(httplib::Response &response, duckdb::Connection &connection,
                   const std::vector<QueryDefinition> &definitions) {
	JsonObject body;
	auto queries = body.SetArray("queries");
	for (const auto &definition : definitions) {
		queries.AddObject(Summary(definition, connection));
	}
	SendJson(response, 200, body.Serialize());
}

void ListRoute(const httplib::Request &request, httplib::Response &response, duckdb::Connection &connection) {
	SendSummaries(response, connection, ListDefinitions(connection, UrlParam(request, "workbook")));
}

//! Without source_kind, the definition §10.2 resolves the name to, with the
//! workbook's queries first when `workbook` is given.
void GetRoute(const httplib::Request &request, httplib::Response &response, duckdb::Connection &connection) {
	const auto id = request.matches[1].str();
	QueryScope scope;
	scope.workbook_id = UrlParam(request, "workbook");
	scope.source_kind = UrlParam(request, "source_kind");
	scope.source_ref = UrlParam(request, "source_ref");
	if (!scope.source_kind.empty() && scope.source_kind != SOURCE_PROJECT && scope.source_kind != SOURCE_WORKBOOK &&
	    scope.source_kind != SOURCE_EXAMPLE) {
		throw ApiError("E_BAD_REQUEST", 422, "source_kind must be project, workbook or example");
	}
	QueryDefinition definition;
	if (!FindQuery(connection, id, scope, definition)) {
		throw ApiError("E_NOT_FOUND", 404, "No query " + id);
	}
	auto summary = Summary(definition, connection);
	summary.Set("text", definition.text);
	SendJson(response, 200, summary.Serialize());
}

void PutRoute(const httplib::Request &request, httplib::Response &response, duckdb::Connection &connection,
              const std::string &projects_dir) {
	const auto document = ParseBody(request);
	const auto body = document.Root();
	const auto project = RequiredString(body, "project");
	const auto text = RequiredString(body, "text");
	const auto saved = SaveProjectQuery(connection, projects_dir, project, request.matches[1].str(), text);
	SendJson(response, 200, Summary(saved, connection).Serialize());
}

void PutWorkbookRoute(const httplib::Request &request, httplib::Response &response, duckdb::Connection &connection) {
	const auto document = ParseBody(request);
	const auto queries = document.Root().Get("queries");
	if (!queries.Is(JsonKind::ARRAY)) {
		throw ApiError("E_BAD_REQUEST", 422, "The request body has no queries array");
	}
	std::vector<std::string> texts;
	for (size_t i = 0; i < queries.Size(); i++) {
		texts.push_back(RequiredString(queries.At(i), "text"));
	}
	SendSummaries(response, connection, RegisterWorkbook(connection, request.matches[1].str(), texts));
}

struct Problem {
	std::string code;
	std::string message;
	int line = 0; //!< 0: not tied to a line of the file
};

//! The parse (§2), then the body: the renderer's text checks for oracle
//! (§4.1), the validator for local (§7). Nothing is registered or run.
void ValidateRoute(const httplib::Request &request, httplib::Response &response, duckdb::Connection &connection) {
	const auto document = ParseBody(request);
	const auto text = RequiredString(document.Root(), "text");
	const auto parsed = ParseQueryFile(text);
	const auto &header = parsed.header;

	std::vector<Problem> errors;
	for (const auto &error : parsed.errors) {
		errors.push_back({error.code, error.message, error.line});
	}
	std::vector<std::string> used;
	if (parsed.Ok() && header.engine == Engine::ORACLE) {
		const auto check = CheckOracleText(header.body, header.params);
		used = check.params_in_text;
		if (!check.ok) {
			errors.push_back({check.code, check.message});
		}
	} else if (parsed.Ok()) {
		const auto verdict = ValidateLocalSql(header.body, LocalSqlScope::FromHeader(header));
		used = verdict.params_in_text;
		if (!verdict.ok) {
			errors.push_back({verdict.code, verdict.message});
		}
	}

	JsonObject body;
	body.Set("ok", errors.empty());
	if (parsed.Ok()) {
		// The definition as registered, if it is; otherwise as a new one.
		const auto hash = DefinitionHash(text);
		QueryDefinition definition;
		if (!FindDefinitionByHash(connection, hash, definition)) {
			definition.definition_hash = hash;
			definition.source_kind = SOURCE_WORKBOOK;
		}
		body.Set("header", Summary(definition, header, connection));
	}
	auto params_in_text = body.SetArray("params_in_text");
	for (const auto &name : used) {
		params_in_text.AddString(name);
	}
	auto error_list = body.SetArray("errors");
	for (const auto &error : errors) {
		JsonObject item;
		item.Set("code", error.code).Set("message", error.message);
		if (error.line > 0) {
			item.Set("line", int64_t(error.line));
		}
		error_list.AddObject(item);
	}
	auto warnings = body.SetArray("warnings");
	if (parsed.Ok()) {
		for (const auto &param : header.params) {
			if (std::find(used.begin(), used.end(), param.name) == used.end()) {
				JsonObject warning;
				warning.Set("code", WARNING_PARAM_UNUSED)
				    .Set("message", "parameter " + param.name + " is declared but not used in the query text");
				warnings.AddObject(warning);
			}
		}
	}
	SendJson(response, 200, body.Serialize());
}

} // namespace

void RegisterQueryRoutes(httplib::Server &server, const RouteContext &context) {
	const auto projects_dir = context.options.projects_dir;
	server.Get("/v1/queries", WithConnection(context, ListRoute));
	server.Post("/v1/queries/validate", WithConnection(context, ValidateRoute));
	server.Get(R"(/v1/queries/([^/]+))", WithConnection(context, GetRoute));
	server.Put(R"(/v1/queries/([^/]+))",
	           WithConnection(context, [projects_dir](const httplib::Request &request, httplib::Response &response,
	                                                  duckdb::Connection &connection) {
		           PutRoute(request, response, connection, projects_dir);
	           }));
	server.Put(R"(/v1/workbooks/([^/]+)/queries)", WithConnection(context, PutWorkbookRoute));
}

} // namespace ofexcel
