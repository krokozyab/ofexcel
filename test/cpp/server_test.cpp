// Starts the HTTP server inside an in-memory database and talks to it over
// 127.0.0.1 on an ephemeral port: the guard, CORS, the health route and the
// server's lifecycle, end to end through httplib.

#include "test_support.hpp"

#include <httplib.h>

#include <string>

using test_support::Contains;
using test_support::Run;
using test_support::RunOk;

namespace {

const char *const ORIGIN = "https://app.example.com";

int PortOf(const std::string &url) {
	return std::stoi(url.substr(url.rfind(':') + 1));
}

std::string StartServer(duckdb::Connection &connection) {
	RunOk(connection, "SET ofexcel_profile = 'hosted'");
	RunOk(connection, std::string("SET ofexcel_hosted_origin = '") + ORIGIN + "'");
	const auto started = RunOk(connection, "SELECT url FROM ofexcel_start(port := 0)");
	CHECK(started.rows.size() == 1);
	const auto url = started.At(0, 0).ToString();
	CHECK(url.rfind("http://127.0.0.1:", 0) == 0);
	return url;
}

void TestHealthAnswersTheHostedOrigin(int port) {
	httplib::Client client("127.0.0.1", port);
	const auto response = client.Get("/v1/health", httplib::Headers {{"Origin", ORIGIN}});
	CHECK(response);
	CHECK(response->status == 200);
	CHECK(Contains(response->body, "\"product\":\"qexcel\""));
	CHECK(Contains(response->body, "\"api\":{\"version\":\"1.0\",\"min_client\":\"1.0\"}"));
	CHECK(Contains(response->body, "\"profile\":\"hosted\""));
	CHECK(Contains(response->body, "\"data_version\":0"));
	CHECK(response->get_header_value("Access-Control-Allow-Origin") == ORIGIN);
	CHECK(response->get_header_value("Vary") == "Origin");
	CHECK(response->get_header_value("X-OF-Data-Version") == "0");
	CHECK(response->has_header("X-OF-Catalog-Version"));
	CHECK(Contains(response->get_header_value("Content-Type"), "application/json"));
}

void TestForeignOriginIsRefusedWithoutCors(int port) {
	httplib::Client client("127.0.0.1", port);
	const auto response = client.Get("/v1/health", httplib::Headers {{"Origin", "https://evil.example.com"}});
	CHECK(response);
	CHECK(response->status == 403);
	CHECK(Contains(response->body, "\"code\":\"E_ORIGIN\""));
	CHECK(!response->has_header("Access-Control-Allow-Origin"));
}

void TestMissingOriginIsRefused(int port) {
	httplib::Client client("127.0.0.1", port);
	const auto response = client.Get("/v1/health");
	CHECK(response);
	CHECK(response->status == 403);
	CHECK(Contains(response->body, "\"code\":\"E_ORIGIN\""));
}

void TestForeignHostIsRefused(int port) {
	httplib::Client client("127.0.0.1", port);
	const auto response = client.Get("/v1/health", httplib::Headers {{"Origin", ORIGIN}, {"Host", "evil.example.com"}});
	CHECK(response);
	CHECK(response->status == 403);
	CHECK(Contains(response->body, "\"code\":\"E_HOST\""));
}

void TestPreflightAllowsTheHostedOrigin(int port) {
	httplib::Client client("127.0.0.1", port);
	const auto response = client.Options("/v1/health", httplib::Headers {{"Origin", ORIGIN},
	                                                    {"Access-Control-Request-Method", "POST"},
	                                                    {"Access-Control-Request-Headers", "content-type"}});
	CHECK(response);
	CHECK(response->status == 204);
	CHECK(response->get_header_value("Access-Control-Allow-Origin") == ORIGIN);
	CHECK(response->get_header_value("Access-Control-Allow-Private-Network") == "true");
	CHECK(Contains(response->get_header_value("Access-Control-Allow-Headers"), "X-OF-Workbook"));
}

void TestUnknownRouteIsJson404(int port) {
	httplib::Client client("127.0.0.1", port);
	const auto response = client.Get("/v1/nope", httplib::Headers {{"Origin", ORIGIN}});
	CHECK(response);
	CHECK(response->status == 404);
	CHECK(Contains(response->body, "\"code\":\"E_NOT_FOUND\""));
}

void TestSecondStartIsRefused(duckdb::Connection &connection) {
	const auto again = Run(connection, "SELECT * FROM ofexcel_start(port := 0)");
	CHECK(again.HasError());
	CHECK(Contains(again.error, "E_CONFLICT"));
}

void TestStatusReportsTheServer(duckdb::Connection &connection, const std::string &url) {
	const auto status = RunOk(connection, "SELECT key, value FROM ofexcel_status()");
	bool saw_running = false;
	bool saw_url = false;
	for (const auto &row : status.rows) {
		if (row[0].ToString() == "server_running") {
			saw_running = row[1].ToString() == "true";
		}
		if (row[0].ToString() == "url") {
			saw_url = row[1].ToString() == url;
		}
	}
	CHECK(saw_running);
	CHECK(saw_url);
}

void TestMigrationsRan(duckdb::Connection &connection) {
	const auto version = RunOk(connection, "SELECT max(version) FROM meta.schema_version");
	CHECK(version.At(0, 0).GetValue<int32_t>() == 1);
	const auto state = RunOk(connection, "SELECT value FROM meta.state WHERE key = 'data_version'");
	CHECK(state.At(0, 0).ToString() == "0");
}

void TestStopAndRestart(duckdb::Connection &connection, int port) {
	const auto stopped = RunOk(connection, "SELECT stopped FROM ofexcel_stop()");
	CHECK(stopped.At(0, 0).GetValue<bool>());
	httplib::Client client("127.0.0.1", port);
	client.set_connection_timeout(1);
	CHECK(!client.Get("/v1/health", httplib::Headers {{"Origin", ORIGIN}}));

	const auto stopped_again = RunOk(connection, "SELECT stopped FROM ofexcel_stop()");
	CHECK(!stopped_again.At(0, 0).GetValue<bool>());

	// A restart runs the migrations again, and they must leave version 1 alone.
	const auto url = RunOk(connection, "SELECT url FROM ofexcel_start(port := 0)").At(0, 0).ToString();
	TestMigrationsRan(connection);
	RunOk(connection, "SELECT stopped FROM ofexcel_stop()");
	(void)url;
}

void TestRefusedConfigurations() {
	test_support::TestDatabase db;
	duckdb::Connection connection(db);

	// Hosted with no origin configured.
	auto refused = Run(connection, "SELECT * FROM ofexcel_start(port := 0)");
	CHECK(refused.HasError());
	CHECK(Contains(refused.error, "ofexcel_hosted_origin"));

	RunOk(connection, "SET ofexcel_hosted_origin = 'https://app.example.com/addin'");
	refused = Run(connection, "SELECT * FROM ofexcel_start(port := 0)");
	CHECK(refused.HasError());
	CHECK(Contains(refused.error, "E_BAD_REQUEST"));

	RunOk(connection, "SET ofexcel_profile = 'local'");
	refused = Run(connection, "SELECT * FROM ofexcel_start(port := 0)");
	CHECK(refused.HasError());
	CHECK(Contains(refused.error, "E_UNSUPPORTED"));

	RunOk(connection, "SET ofexcel_profile = 'remote'");
	refused = Run(connection, "SELECT * FROM ofexcel_start(port := 0)");
	CHECK(refused.HasError());
	CHECK(Contains(refused.error, "ofexcel_profile"));
}

} // namespace

int main() {
	TestRefusedConfigurations();

	test_support::TestDatabase db;
	duckdb::Connection connection(db);
	const auto url = StartServer(connection);
	const auto port = PortOf(url);

	TestHealthAnswersTheHostedOrigin(port);
	TestForeignOriginIsRefusedWithoutCors(port);
	TestMissingOriginIsRefused(port);
	TestForeignHostIsRefused(port);
	TestPreflightAllowsTheHostedOrigin(port);
	TestUnknownRouteIsJson404(port);
	TestSecondStartIsRefused(connection);
	TestStatusReportsTheServer(connection, url);
	TestMigrationsRan(connection);
	TestStopAndRestart(connection, port);

	std::cout << "server_test: all checks passed\n";
	return 0;
}
