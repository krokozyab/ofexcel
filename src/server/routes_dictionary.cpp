// Fusion's dictionary for the SQL editor (CONTRACTS.md §8.3 /v1/dictionary/*;
// TASKS.md C9): tables and views by name, and a table's columns, through
// ofquack's oracle_fusion_tables() and oracle_fusion_columns(). ofquack caches
// both, so a lookup after the first costs no BI Publisher call.

#include "routes.hpp"

#include "ofexcel/api_error.hpp"
#include "ofexcel/connections.hpp"
#include "ofexcel/db_util.hpp"
#include "ofexcel/fusion_errors.hpp"
#include "ofexcel/json_body.hpp"

#include <algorithm>
#include <cctype>
#include <string>

namespace ofexcel {

namespace {

constexpr int64_t DEFAULT_TABLES_LISTED = 50;
constexpr int64_t MAX_TABLES_LISTED = 1000;

std::string UrlParam(const httplib::Request &request, const char *name) {
	return request.has_param(name) ? request.get_param_value(name) : "";
}

//! The connection named in the URL, which must be a Fusion secret.
std::string Connection(const httplib::Request &request, duckdb::Connection &connection) {
	const auto name = UrlParam(request, "connection");
	if (name.empty()) {
		throw ApiError("E_BAD_REQUEST", 422, "connection is required");
	}
	if (!FusionSecretExists(connection, name)) {
		throw ApiError("E_NOT_FOUND", 404, "No connection " + name);
	}
	return name;
}

//! ofquack's error as §14.1 maps it. A table the dictionary does not have is
//! E_NOT_FOUND.
ApiError DictionaryError(const duckdb::ErrorData &error) {
	if (error.Type() == duckdb::ExceptionType::BINDER &&
	    error.RawMessage().find("has no table or view named") != std::string::npos) {
		return ApiError("E_NOT_FOUND", 404, error.RawMessage());
	}
	return FusionApiError(error);
}

std::vector<Row> Dictionary(duckdb::Connection &connection, const std::string &sql,
                            duckdb::vector<duckdb::Value> params) {
	auto outcome = TryQueryRows(connection, sql, std::move(params));
	if (!outcome.Ok()) {
		throw DictionaryError(outcome.error);
	}
	return std::move(outcome.rows);
}

//! Names containing `q`, without regard to case; those that start with it
//! first, then by name.
void TablesRoute(const httplib::Request &request, httplib::Response &response, duckdb::Connection &connection) {
	const auto secret = Connection(request, connection);
	const auto q = UrlParam(request, "q");
	const auto limit_text = UrlParam(request, "limit");
	int64_t limit = DEFAULT_TABLES_LISTED;
	if (!limit_text.empty()) {
		const bool digits = limit_text.size() <= 4 && std::all_of(limit_text.begin(), limit_text.end(),
		                                                          [](unsigned char c) { return std::isdigit(c); });
		limit = digits ? std::stoll(limit_text) : 0;
		if (limit < 1 || limit > MAX_TABLES_LISTED) {
			throw ApiError("E_BAD_REQUEST", 422,
			               "limit must be an integer from 1 to " + std::to_string(MAX_TABLES_LISTED));
		}
	}
	const auto rows = Dictionary(connection,
	                             "SELECT table_name, remarks FROM oracle_fusion_tables(secret := $1) "
	                             "WHERE contains(upper(table_name), upper($2)) "
	                             "ORDER BY NOT starts_with(upper(table_name), upper($2)), upper(table_name) LIMIT $3",
	                             {duckdb::Value(secret), duckdb::Value(q), duckdb::Value::BIGINT(limit)});
	JsonObject body;
	auto tables = body.SetArray("tables");
	for (const auto &row : rows) {
		JsonObject table;
		table.Set("name", row[0].ToString());
		if (!row[1].IsNull() && !row[1].ToString().empty()) {
			table.Set("description", row[1].ToString());
		}
		tables.AddObject(table);
	}
	SendJson(response, 200, body.Serialize());
}

//! In column order. `type` is the Oracle type, with precision and scale for a
//! NUMBER that has them.
void ColumnsRoute(const httplib::Request &request, httplib::Response &response, duckdb::Connection &connection) {
	const auto secret = Connection(request, connection);
	const auto table = UrlParam(request, "table");
	if (table.empty()) {
		throw ApiError("E_BAD_REQUEST", 422, "table is required");
	}
	const auto rows = Dictionary(connection,
	                             "SELECT column_name, oracle_type, precision, scale, remarks "
	                             "FROM oracle_fusion_columns($1, secret := $2) ORDER BY ordinal",
	                             {duckdb::Value(table), duckdb::Value(secret)});
	JsonObject body;
	auto columns = body.SetArray("columns");
	for (const auto &row : rows) {
		JsonObject column;
		column.Set("name", row[0].ToString());
		if (!row[1].IsNull() && !row[1].ToString().empty()) {
			auto type = row[1].ToString();
			const auto precision = row[2].IsNull() ? 0 : row[2].GetValue<int64_t>();
			if (type == "NUMBER" && precision > 0) {
				type += "(" + std::to_string(precision) + "," +
				        std::to_string(row[3].IsNull() ? 0 : row[3].GetValue<int64_t>()) + ")";
			}
			column.Set("type", type);
		}
		if (!row[4].IsNull() && !row[4].ToString().empty()) {
			column.Set("description", row[4].ToString());
		}
		columns.AddObject(column);
	}
	SendJson(response, 200, body.Serialize());
}

} // namespace

void RegisterDictionaryRoutes(httplib::Server &server, const RouteContext &context) {
	server.Get("/v1/dictionary/tables", WithConnection(context, TablesRoute));
	server.Get("/v1/dictionary/columns", WithConnection(context, ColumnsRoute));
}

} // namespace ofexcel
