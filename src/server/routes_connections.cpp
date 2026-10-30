// Connections to Fusion and signing in to them (CONTRACTS.md §8.3, TASKS.md A4).

#include "routes.hpp"

#include "ofexcel/api_error.hpp"
#include "ofexcel/connections.hpp"
#include "ofexcel/db_util.hpp"
#include "ofexcel/fusion_errors.hpp"
#include "ofexcel/json_body.hpp"
#include "ofexcel/query_file.hpp"

#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>

namespace ofexcel {

namespace {

//! What /v1/connections/{name}/test runs through §5.1.
constexpr const char *TEST_SQL = "SELECT 1 FROM dual";

using ConnectionHandler =
    std::function<void(const httplib::Request &, httplib::Response &, duckdb::Connection &)>;

//! Opens the request's connection, sets the version headers, and turns an
//! ApiError into its error body.
httplib::Server::Handler WithConnection(const RouteContext &context, ConnectionHandler handler) {
	return [context, handler](const httplib::Request &request, httplib::Response &response) {
		auto connection = context.Connect();
		if (!connection) {
			SendError(response, 503, "E_INTERNAL", "База данных закрыта");
			return;
		}
		SetVersionHeaders(response, *connection);
		try {
			handler(request, response, *connection);
		} catch (const ApiError &error) {
			SendError(response, error.status, error.code, error.what());
		}
	};
}

JsonDocument ParseBody(const httplib::Request &request) {
	try {
		auto document = JsonDocument::Parse(request.body);
		if (document.Root().Is(JsonKind::OBJECT)) {
			return document;
		}
	} catch (const std::invalid_argument &) {
	}
	throw ApiError("E_BAD_REQUEST", 400, "Тело запроса должно быть объектом JSON");
}

std::string RequiredString(const JsonValue &body, const std::string &key) {
	const auto value = body.Get(key);
	if (!value.Is(JsonKind::STRING)) {
		throw ApiError("E_BAD_REQUEST", 422, "В теле запроса нет строки " + key);
	}
	return value.String();
}

//! The name of an oracle_fusion secret, or E_NOT_FOUND. Checked before the
//! name reaches ofquack, whose own error would read as a Fusion failure.
std::string ExistingConnection(duckdb::Connection &connection, const std::string &name) {
	if (!IsIdentifier(name) || !FusionSecretExists(connection, name)) {
		throw ApiError("E_NOT_FOUND", 404, "Нет подключения " + name);
	}
	return name;
}

JsonObject ConnectionJson(const FusionConnection &definition) {
	JsonObject json;
	json.Set("name", definition.name)
	    .Set("environment", definition.environment)
	    .Set("endpoint", definition.endpoint)
	    .Set("report_path", definition.report_path);
	return json;
}

struct SignInState {
	bool signed_in = false;
	std::string user; //!< "" when ofquack does not know the subject
};

//! From fusion_scanner_sso_status(): never the token, only whether there is one.
SignInState ReadSignInState(duckdb::Connection &connection, const std::string &name) {
	SignInState state;
	const auto outcome = TryQueryRows(
	    connection, "SELECT have_token, subject FROM fusion_scanner_sso_status(secret := $1)", {duckdb::Value(name)});
	if (outcome.Ok() && !outcome.rows.empty()) {
		const auto &row = outcome.rows[0];
		state.signed_in = !row[0].IsNull() && row[0].GetValue<bool>();
		state.user = row[1].IsNull() ? "" : row[1].ToString();
	}
	return state;
}

//! One sign-in per connection at a time: a second request waits for the first
//! and then finds its token instead of opening another browser window.
std::shared_ptr<std::mutex> SignInLock(const std::string &name) {
	static std::mutex guard;
	static std::map<std::string, std::shared_ptr<std::mutex>> locks;
	std::lock_guard<std::mutex> lock(guard);
	auto &entry = locks[name];
	if (!entry) {
		entry = std::make_shared<std::mutex>();
	}
	return entry;
}

void Status(const httplib::Request &, httplib::Response &response, duckdb::Connection &connection) {
	JsonObject body;
	auto connections = body.SetArray("connections");
	for (const auto &definition : ListConnections(connection)) {
		const auto state = ReadSignInState(connection, definition.name);
		JsonObject item;
		item.Set("name", definition.name)
		    .Set("environment", definition.environment)
		    .Set("host", EndpointHost(definition.endpoint))
		    .Set("signed_in", state.signed_in);
		if (!state.user.empty()) {
			item.Set("user", state.user);
		}
		connections.AddObject(item);
	}
	// Runs arrive with the run engine (TASKS.md C3); until then there are none.
	body.Set("active_runs", int64_t(0));
	body.SetArray("warnings");
	SendJson(response, 200, body.Serialize());
}

void ListRoute(const httplib::Request &, httplib::Response &response, duckdb::Connection &connection) {
	JsonObject body;
	auto connections = body.SetArray("connections");
	for (const auto &definition : ListConnections(connection)) {
		connections.AddObject(ConnectionJson(definition));
	}
	SendJson(response, 200, body.Serialize());
}

void CreateRoute(const httplib::Request &request, httplib::Response &response, duckdb::Connection &connection) {
	const auto document = ParseBody(request);
	const auto body = document.Root();
	FusionConnection definition;
	definition.name = RequiredString(body, "name");
	definition.environment = RequiredString(body, "environment");
	definition.endpoint = RequiredString(body, "endpoint");
	definition.report_path = RequiredString(body, "report_path");
	SendJson(response, 201, ConnectionJson(CreateConnection(connection, definition)).Serialize());
}

//! `SELECT 1 FROM dual` exactly as a run would send it (§5.1). A failure is the
//! answer, not an error: 200 with ok false and the §14.1 code.
void TestRoute(const httplib::Request &request, httplib::Response &response, duckdb::Connection &connection) {
	const auto name = ExistingConnection(connection, request.matches[1].str());
	const auto outcome = TryQueryRows(connection,
	                                  "SELECT * FROM oracle_fusion_query($1, secret := $2, all_varchar := true, "
	                                  "on_cast_error := 'error', fetch_size := 10000)",
	                                  {duckdb::Value(TEST_SQL), duckdb::Value(name)});
	JsonObject body;
	if (outcome.Ok()) {
		body.Set("ok", true);
		const auto state = ReadSignInState(connection, name);
		if (!state.user.empty()) {
			body.Set("user", state.user);
		}
	} else {
		const auto error = FusionApiError(outcome.error);
		JsonObject detail;
		detail.Set("code", error.code).Set("message", error.what());
		body.Set("ok", false).Set("error", detail);
	}
	SendJson(response, 200, body.Serialize());
}

//! Opens a browser window and waits for the person, up to SSO_TIMEOUT_SECONDS
//! (ofquack's own limit, set on the secret). Blocks this request's thread for
//! that long; the client waits at least 310 s (§8.3).
void LoginRoute(const httplib::Request &request, httplib::Response &response, duckdb::Connection &connection) {
	const auto document = ParseBody(request);
	const auto name = ExistingConnection(connection, RequiredString(document.Root(), "connection"));
	const auto lock = SignInLock(name);
	std::lock_guard<std::mutex> signing_in(*lock);
	const auto outcome =
	    TryQueryRows(connection, "SELECT subject FROM fusion_scanner_sso_login(secret := $1)", {duckdb::Value(name)});
	if (!outcome.Ok()) {
		// Whatever stopped it -- the timeout, a closed window, no browser -- the
		// person is not signed in, which is E_FUSION_AUTH.
		std::cerr << "ofexceld: sign-in to " << name << " failed: " << outcome.error.RawMessage() << std::endl;
		const auto &raw = outcome.error.RawMessage();
		throw ApiError("E_FUSION_AUTH", 409, "Вход в Oracle Fusion не завершён: " + raw.substr(0, raw.find('\n')));
	}
	JsonObject body;
	const auto &subject = outcome.rows.empty() ? duckdb::Value() : outcome.rows[0][0];
	body.Set("user", subject.IsNull() ? std::string() : subject.ToString());
	SendJson(response, 200, body.Serialize());
}

void LogoutRoute(const httplib::Request &request, httplib::Response &response, duckdb::Connection &connection) {
	const auto document = ParseBody(request);
	const auto name = ExistingConnection(connection, RequiredString(document.Root(), "connection"));
	QueryRows(connection, "SELECT * FROM fusion_scanner_sso_logout(secret := $1)", {duckdb::Value(name)});
	response.status = 204;
}

} // namespace

void RegisterConnectionRoutes(httplib::Server &server, const RouteContext &context) {
	server.Get("/v1/status", WithConnection(context, Status));
	server.Get("/v1/connections", WithConnection(context, ListRoute));
	server.Post("/v1/connections", WithConnection(context, CreateRoute));
	server.Post(R"(/v1/connections/([^/]+)/test)", WithConnection(context, TestRoute));
	server.Post("/v1/auth/login", WithConnection(context, LoginRoute));
	server.Post("/v1/auth/logout", WithConnection(context, LogoutRoute));
}

} // namespace ofexcel
