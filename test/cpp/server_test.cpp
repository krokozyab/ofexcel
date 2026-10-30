// Starts the HTTPS server inside an in-memory database and talks to it over
// 127.0.0.1 on an ephemeral port: TLS, the guard, CORS, the health route and
// the server's lifecycle, end to end through httplib.
//
//   ofexcel_server_test <path to ofexceld>
//
// The certificates come from the ofexceld binary itself, `install-cert
// --no-trust --dir <temporary>`, and the client verifies them against that
// ca.crt only.

#include "ofexcel/local_ca.hpp"
#include "ofexcel/paths.hpp"
#include "temporary_directory.hpp"
#include "test_support.hpp"

#include <httplib.h>

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>

#ifndef _WIN32
#include <sys/stat.h>
#endif

using test_support::Contains;
using test_support::Run;
using test_support::RunOk;

namespace {

const char *const ORIGIN = "https://app.example.com";

//! Set once in main: the directory install-cert filled.
ofexcel::TlsFiles tls;

int PortOf(const std::string &url) {
	return std::stoi(url.substr(url.rfind(':') + 1));
}

//! `ofexceld install-cert --no-trust --dir <dir>`, through the shell.
int RunInstallCert(const std::string &ofexceld, const std::string &dir) {
#ifdef _WIN32
	// cmd.exe drops the outer pair of quotes when the line starts with one.
	const auto command = "\"\"" + ofexceld + "\" install-cert --no-trust --dir \"" + dir + "\"\"";
#else
	const auto command = "'" + ofexceld + "' install-cert --no-trust --dir '" + dir + "'";
#endif
	return std::system(command.c_str());
}

//! A client that trusts only the test's own CA and checks the server's name.
std::unique_ptr<httplib::SSLClient> Client(int port, const std::string &host = "127.0.0.1") {
	auto client = std::make_unique<httplib::SSLClient>(host, port);
	client->set_ca_cert_path(tls.ca_cert.c_str());
	client->enable_server_certificate_verification(true);
	client->enable_server_hostname_verification(true);
	// localhost may resolve to ::1 first, and the server listens on 127.0.0.1 only.
	client->set_hostname_addr_map({{"localhost", "127.0.0.1"}});
	client->set_connection_timeout(5);
	return client;
}

std::string SqlString(const std::string &value) {
	std::string quoted = "'";
	for (const auto c : value) {
		quoted += c;
		if (c == '\'') {
			quoted += '\'';
		}
	}
	return quoted + "'";
}

std::string StartServer(duckdb::Connection &connection) {
	RunOk(connection, "SET ofexcel_profile = 'hosted'");
	RunOk(connection, std::string("SET ofexcel_hosted_origin = '") + ORIGIN + "'");
	RunOk(connection, "SET ofexcel_tls_dir = " + SqlString(tls.dir));
	const auto started = RunOk(connection, "SELECT url FROM ofexcel_start(port := 0)");
	CHECK(started.rows.size() == 1);
	const auto url = started.At(0, 0).ToString();
	CHECK(url.rfind("https://127.0.0.1:", 0) == 0);
	return url;
}

void TestInstallCertWroteTheFiles() {
	for (const auto &path : {tls.ca_cert, tls.ca_key, tls.server_cert, tls.server_key}) {
		CHECK(std::filesystem::exists(std::filesystem::u8path(path)));
	}
#ifndef _WIN32
	struct stat info;
	CHECK(::stat(tls.ca_key.c_str(), &info) == 0 && (info.st_mode & 0777) == 0600);
	CHECK(::stat(tls.server_key.c_str(), &info) == 0 && (info.st_mode & 0777) == 0600);
#endif
}

void TestHealthAnswersTheHostedOrigin(int port) {
	const auto response = Client(port)->Get("/v1/health", httplib::Headers {{"Origin", ORIGIN}});
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

void TestLocalhostNameVerifies(int port) {
	// The certificate's DNS name, as the add-in's https://localhost:<port> uses it.
	const auto response = Client(port, "localhost")->Get("/v1/health", httplib::Headers {{"Origin", ORIGIN}});
	CHECK(response);
	CHECK(response->status == 200);
}

void TestClientWithoutTheCaIsRefused(int port) {
	httplib::SSLClient client("127.0.0.1", port);
	client.enable_server_certificate_verification(true);
	client.set_connection_timeout(5);
	const auto response = client.Get("/v1/health", httplib::Headers {{"Origin", ORIGIN}});
	CHECK(!response);
	CHECK(response.error() == httplib::Error::SSLServerVerification);
}

void TestPlainHttpIsRefused(int port) {
	httplib::Client client("127.0.0.1", port);
	client.set_connection_timeout(5);
	client.set_read_timeout(5);
	const auto response = client.Get("/v1/health", httplib::Headers {{"Origin", ORIGIN}});
	CHECK(!response);
}

void TestForeignOriginIsRefusedWithoutCors(int port) {
	const auto response = Client(port)->Get("/v1/health", httplib::Headers {{"Origin", "https://evil.example.com"}});
	CHECK(response);
	CHECK(response->status == 403);
	CHECK(Contains(response->body, "\"code\":\"E_ORIGIN\""));
	CHECK(!response->has_header("Access-Control-Allow-Origin"));
}

void TestMissingOriginIsRefused(int port) {
	const auto response = Client(port)->Get("/v1/health");
	CHECK(response);
	CHECK(response->status == 403);
	CHECK(Contains(response->body, "\"code\":\"E_ORIGIN\""));
}

void TestForeignHostIsRefused(int port) {
	const auto response =
	    Client(port)->Get("/v1/health", httplib::Headers {{"Origin", ORIGIN}, {"Host", "evil.example.com"}});
	CHECK(response);
	CHECK(response->status == 403);
	CHECK(Contains(response->body, "\"code\":\"E_HOST\""));
}

void TestPreflightAllowsTheHostedOrigin(int port) {
	const auto response = Client(port)->Options("/v1/health", httplib::Headers {{"Origin", ORIGIN},
	                                                           {"Access-Control-Request-Method", "POST"},
	                                                           {"Access-Control-Request-Headers", "content-type"}});
	CHECK(response);
	CHECK(response->status == 204);
	CHECK(response->get_header_value("Access-Control-Allow-Origin") == ORIGIN);
	CHECK(response->get_header_value("Access-Control-Allow-Private-Network") == "true");
	CHECK(Contains(response->get_header_value("Access-Control-Allow-Headers"), "X-OF-Workbook"));
}

void TestUnknownRouteIsJson404(int port) {
	const auto response = Client(port)->Get("/v1/nope", httplib::Headers {{"Origin", ORIGIN}});
	CHECK(response);
	CHECK(response->status == 404);
	CHECK(Contains(response->body, "\"code\":\"E_NOT_FOUND\""));
}

void TestAddinIsNotServedInHosted(int port) {
	// The hosted add-in comes from its own HTTPS host; the daemon serves only the API.
	const auto response = Client(port)->Get("/addin/taskpane.html", httplib::Headers {{"Origin", ORIGIN}});
	CHECK(response);
	CHECK(response->status == 404);
	CHECK(response->body.find("qexcel-session") == std::string::npos);
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
	bool saw_tls_dir = false;
	for (const auto &row : status.rows) {
		if (row[0].ToString() == "server_running") {
			saw_running = row[1].ToString() == "true";
		}
		if (row[0].ToString() == "url") {
			saw_url = row[1].ToString() == url;
		}
		if (row[0].ToString() == "tls_dir") {
			saw_tls_dir = row[1].ToString() == tls.dir;
		}
	}
	CHECK(saw_running);
	CHECK(saw_url);
	CHECK(saw_tls_dir);
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
	CHECK(!Client(port)->Get("/v1/health", httplib::Headers {{"Origin", ORIGIN}}));

	const auto stopped_again = RunOk(connection, "SELECT stopped FROM ofexcel_stop()");
	CHECK(!stopped_again.At(0, 0).GetValue<bool>());

	// A restart runs the migrations again, and they must leave version 1 alone.
	RunOk(connection, "SELECT url FROM ofexcel_start(port := 0)");
	TestMigrationsRan(connection);
	RunOk(connection, "SELECT stopped FROM ofexcel_stop()");
}

void TestStartRenewsAnExpiringCertificate(duckdb::Connection &connection) {
	const auto ca_before = ofexcel::ReadFileBytes(tls.ca_cert);
	ofexcel::IssueServerCertificate(tls, ofexcel::SERVER_RENEW_DAYS - 1);
	const auto expiring = ofexcel::ReadFileBytes(tls.server_cert);

	const auto url = RunOk(connection, "SELECT url FROM ofexcel_start(port := 0)").At(0, 0).ToString();
	CHECK(ofexcel::ReadFileBytes(tls.server_cert) != expiring);
	CHECK(ofexcel::ReadFileBytes(tls.ca_cert) == ca_before);
	// Renewed for the full term: loading it again leaves it alone.
	CHECK(!ofexcel::LoadServerCertificate(tls.dir).renewed);
	// And the server serves the renewed certificate, which the same CA vouches for.
	const auto response = Client(PortOf(url))->Get("/v1/health", httplib::Headers {{"Origin", ORIGIN}});
	CHECK(response);
	CHECK(response->status == 200);
	RunOk(connection, "SELECT stopped FROM ofexcel_stop()");
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

	// No CA in the directory: the message says what to run.
	test_support::TemporaryDirectory empty("ofexcel_server_test_empty");
	RunOk(connection, std::string("SET ofexcel_hosted_origin = '") + ORIGIN + "'");
	RunOk(connection, "SET ofexcel_tls_dir = " + SqlString(empty.path));
	refused = Run(connection, "SELECT * FROM ofexcel_start(port := 0)");
	CHECK(refused.HasError());
	CHECK(Contains(refused.error, "E_UNSUPPORTED"));
	CHECK(Contains(refused.error, "ofexceld install-cert"));
	CHECK(!std::filesystem::exists(std::filesystem::u8path(empty.path)));

	// The local profile needs no origin, but the same certificates.
	RunOk(connection, "SET ofexcel_profile = 'local'");
	RunOk(connection, "SET ofexcel_hosted_origin = ''");
	refused = Run(connection, "SELECT * FROM ofexcel_start(port := 0)");
	CHECK(refused.HasError());
	CHECK(Contains(refused.error, "ofexceld install-cert"));

	RunOk(connection, "SET ofexcel_profile = 'remote'");
	refused = Run(connection, "SELECT * FROM ofexcel_start(port := 0)");
	CHECK(refused.HasError());
	CHECK(Contains(refused.error, "ofexcel_profile"));
}

} // namespace

int main(int argc, char **argv) {
	if (argc < 2) {
		std::cerr << "usage: ofexcel_server_test <path to ofexceld>\n";
		return 2;
	}
	test_support::TemporaryDirectory directory("ofexcel_server_test");
	tls = ofexcel::TlsFilesIn(ofexcel::JoinPath(directory.path, "tls"));
	CHECK(RunInstallCert(argv[1], tls.dir) == 0);
	TestInstallCertWroteTheFiles();

	TestRefusedConfigurations();

	test_support::TestDatabase db;
	duckdb::Connection connection(db);
	const auto url = StartServer(connection);
	const auto port = PortOf(url);

	TestHealthAnswersTheHostedOrigin(port);
	TestLocalhostNameVerifies(port);
	TestClientWithoutTheCaIsRefused(port);
	TestPlainHttpIsRefused(port);
	TestForeignOriginIsRefusedWithoutCors(port);
	TestMissingOriginIsRefused(port);
	TestForeignHostIsRefused(port);
	TestPreflightAllowsTheHostedOrigin(port);
	TestUnknownRouteIsJson404(port);
	TestAddinIsNotServedInHosted(port);
	TestSecondStartIsRefused(connection);
	TestStatusReportsTheServer(connection, url);
	TestMigrationsRan(connection);
	TestStopAndRestart(connection, port);
	TestStartRenewsAnExpiringCertificate(connection);

	std::cout << "server_test: all checks passed\n";
	return 0;
}
