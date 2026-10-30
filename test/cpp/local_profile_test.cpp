// The local profile on a live HTTPS server (TASKS.md A2): the daemon serves the
// add-in's files under /addin/ with the session token in each page, and the
// API takes the token instead of an Origin. G12–G16 of CONTRACTS.md §8.1.1
// against the real server, not just the pure guard.

#include "ofexcel/local_ca.hpp"
#include "ofexcel/paths.hpp"
#include "temporary_directory.hpp"
#include "test_support.hpp"

#include <httplib.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

using test_support::Contains;
using test_support::Run;
using test_support::RunOk;

namespace fs = std::filesystem;

namespace {

ofexcel::TlsFiles tls;

int PortOf(const std::string &url) {
	return std::stoi(url.substr(url.rfind(':') + 1));
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

//! A client for https://localhost:<port>, the origin the local add-in has.
std::unique_ptr<httplib::SSLClient> Client(int port) {
	auto client = std::make_unique<httplib::SSLClient>("localhost", port);
	client->set_ca_cert_path(tls.ca_cert.c_str());
	client->enable_server_certificate_verification(true);
	client->set_hostname_addr_map({{"localhost", "127.0.0.1"}});
	client->set_connection_timeout(5);
	return client;
}

std::string OwnOrigin(int port) {
	return "https://localhost:" + std::to_string(port);
}

int StartLocal(duckdb::Connection &connection, const std::string &addin_dir = "") {
	RunOk(connection, "SET ofexcel_profile = 'local'");
	RunOk(connection, "SET ofexcel_tls_dir = " + SqlString(tls.dir));
	RunOk(connection, "SET ofexcel_addin_dir = " + SqlString(addin_dir));
	const auto url = RunOk(connection, "SELECT url FROM ofexcel_start(port := 0)").At(0, 0).ToString();
	CHECK(url.rfind("https://127.0.0.1:", 0) == 0);
	return PortOf(url);
}

void Stop(duckdb::Connection &connection) {
	RunOk(connection, "SELECT stopped FROM ofexcel_stop()");
}

//! The token the task pane carries, as the add-in reads it from the page.
std::string TokenFromPage(const std::string &html) {
	const std::string marker = "<meta name=\"qexcel-session\" content=\"";
	const auto at = html.find(marker);
	CHECK(at != std::string::npos);
	const auto start = at + marker.size();
	const auto end = html.find('"', start);
	CHECK(end != std::string::npos);
	return html.substr(start, end - start);
}

std::string FetchToken(int port) {
	const auto page = Client(port)->Get("/addin/taskpane.html");
	CHECK(page);
	CHECK(page->status == 200);
	return TokenFromPage(page->body);
}

void TestTaskPaneCarriesTheToken(int port) {
	const auto page = Client(port)->Get("/addin/taskpane.html");
	CHECK(page);
	CHECK(page->status == 200);
	CHECK(Contains(page->get_header_value("Content-Type"), "text/html"));
	CHECK(page->get_header_value("Cache-Control") == "no-store");
	const auto token = TokenFromPage(page->body);
	CHECK(token.size() == 64);
	CHECK(token.find_first_not_of("0123456789abcdef") == std::string::npos);
	// Right inside <head>, before anything the page itself has there.
	CHECK(Contains(page->body, "<head>\n<meta name=\"qexcel-session\"") ||
	      Contains(page->body, "<head><meta name=\"qexcel-session\""));
	// The page is not guarded by Origin: a navigation sends none, and a foreign
	// one cannot read the body without CORS headers, which it does not get.
	const auto foreign = Client(port)->Get("/addin/taskpane.html", httplib::Headers {{"Origin", "https://evil.example.com"}});
	CHECK(foreign);
	CHECK(foreign->status == 200);
	CHECK(!foreign->has_header("Access-Control-Allow-Origin"));
}

void TestFunctionsJson(int port) {
	const auto response = Client(port)->Get("/addin/functions.json");
	CHECK(response);
	CHECK(response->status == 200);
	CHECK(Contains(response->get_header_value("Content-Type"), "application/json"));
	for (const auto *id : {"\"TABLE\"", "\"CALL\"", "\"ASOF\"", "\"STATUS\""}) {
		CHECK(Contains(response->body, id));
	}
	CHECK(!Contains(response->body, "qexcel-session"));
}

void TestSessionVectors(int port, const std::string &token) {
	const auto origin = OwnOrigin(port);
	// G12: same origin, no Origin header, the token.
	auto response = Client(port)->Get("/v1/health", httplib::Headers {{"X-OF-Session", token}});
	CHECK(response && response->status == 200);
	CHECK(Contains(response->body, "\"profile\":\"local\""));
	CHECK(!response->has_header("Access-Control-Allow-Origin"));
	// G13: our own Origin and the token.
	response = Client(port)->Get("/v1/health", httplib::Headers {{"Origin", origin}, {"X-OF-Session", token}});
	CHECK(response && response->status == 200);
	// G14: a foreign Origin, even with the token.
	response = Client(port)->Get("/v1/health",
	                             httplib::Headers {{"Origin", "https://app.example.com"}, {"X-OF-Session", token}});
	CHECK(response && response->status == 403);
	CHECK(Contains(response->body, "\"code\":\"E_ORIGIN\""));
	// G15: no token.
	response = Client(port)->Get("/v1/health");
	CHECK(response && response->status == 401);
	CHECK(Contains(response->body, "\"code\":\"E_SESSION\""));
	// G16: a wrong one.
	response = Client(port)->Get("/v1/health", httplib::Headers {{"X-OF-Session", "guess"}});
	CHECK(response && response->status == 401);
	CHECK(Contains(response->body, "\"code\":\"E_SESSION\""));
	// A token of the right length that is not ours.
	response = Client(port)->Get("/v1/health", httplib::Headers {{"X-OF-Session", std::string(64, 'a')}});
	CHECK(response && response->status == 401);
}

void TestHostGuardsThePagesToo(int port) {
	// DNS rebinding: a page under another name that resolves to 127.0.0.1 is
	// same-origin with itself and could read the token, but its Host gives it away.
	const auto response = Client(port)->Get("/addin/taskpane.html", httplib::Headers {{"Host", "evil.example.com"}});
	CHECK(response);
	CHECK(response->status == 403);
	CHECK(Contains(response->body, "\"code\":\"E_HOST\""));
	CHECK(!Contains(response->body, "qexcel-session"));
}

void TestPathsCannotLeaveTheAddin(int port) {
	for (const auto *path : {"/addin/../v1/health", "/addin/%2e%2e/v1/health", "/addin/a/../taskpane.html",
	                         "/addin/./taskpane.html", "/addin/", "/addin//taskpane.html", "/addin/..%2ftaskpane.html",
	                         "/addin/..%5ctaskpane.html", "/addin/missing.html", "/addin/C:/Windows/win.ini"}) {
		const auto response = Client(port)->Get(path);
		CHECK(response);
		if (response->status != 404) {
			std::cerr << path << " answered " << response->status << "\n";
		}
		CHECK(response->status == 404);
		CHECK(!Contains(response->body, "qexcel-session"));
	}
}

void TestTokenIsNewOnEveryStart(duckdb::Connection &connection, int &port, std::string &token) {
	Stop(connection);
	port = StartLocal(connection);
	const auto fresh = FetchToken(port);
	CHECK(fresh != token);
	const auto stale = Client(port)->Get("/v1/health", httplib::Headers {{"X-OF-Session", token}});
	CHECK(stale && stale->status == 401);
	token = fresh;
}

void WriteFile(const fs::path &path, const std::string &content) {
	fs::create_directories(path.parent_path());
	std::ofstream file(path, std::ios::binary);
	file << content;
	CHECK(file);
}

void TestAddinDirectory(duckdb::Connection &connection) {
	test_support::TemporaryDirectory temporary("ofexcel_local_profile_addin");
	const auto root = fs::u8path(temporary.path);
	const auto dir = root / "addin";
	WriteFile(dir / "taskpane.html", "<!doctype html><HTML><Head lang=\"ru\"><title>t</title></Head><body>x</body></HTML>");
	WriteFile(dir / "js" / "app.js", "console.log('<head>');");
	WriteFile(dir / "page.htm", "<p>no head here</p>");
	WriteFile(root / "secret.txt", "outside");

	Stop(connection);
	const auto port = StartLocal(connection, dir.u8string());
	const auto page = Client(port)->Get("/addin/taskpane.html");
	CHECK(page && page->status == 200);
	CHECK(Contains(page->body, "<Head lang=\"ru\"><meta name=\"qexcel-session\" content=\""));
	const auto token = TokenFromPage(page->body);

	const auto script = Client(port)->Get("/addin/js/app.js");
	CHECK(script && script->status == 200);
	CHECK(script->body == "console.log('<head>');");
	CHECK(Contains(script->get_header_value("Content-Type"), "javascript"));

	const auto headless = Client(port)->Get("/addin/page.htm");
	CHECK(headless && headless->status == 200);
	CHECK(headless->body.rfind("<meta name=\"qexcel-session\" content=\"" + token + "\">", 0) == 0);

	// The built-in files are not there when a directory is set.
	const auto builtin = Client(port)->Get("/addin/functions.json");
	CHECK(builtin && builtin->status == 404);

	const auto outside = Client(port)->Get("/addin/../secret.txt");
	CHECK(outside && outside->status == 404);
#ifndef _WIN32
	// A symbolic link that points out of the directory is not followed out.
	std::error_code error;
	fs::create_symlink(root / "secret.txt", dir / "link.txt", error);
	CHECK(!error);
	const auto linked = Client(port)->Get("/addin/link.txt");
	CHECK(linked && linked->status == 404);
	CHECK(!Contains(linked->body, "outside"));
#endif
	Stop(connection);
}

} // namespace

int main() {
	test_support::TemporaryDirectory directory("ofexcel_local_profile_test");
	tls = ofexcel::TlsFilesIn(ofexcel::JoinPath(directory.path, "tls"));
	ofexcel::InstallLocalCa(tls.dir);

	test_support::TestDatabase db;
	duckdb::Connection connection(db);
	int port = StartLocal(connection);
	auto token = FetchToken(port);

	TestTaskPaneCarriesTheToken(port);
	TestFunctionsJson(port);
	TestSessionVectors(port, token);
	TestHostGuardsThePagesToo(port);
	TestPathsCannotLeaveTheAddin(port);
	TestTokenIsNewOnEveryStart(connection, port, token);
	TestSessionVectors(port, token);
	TestAddinDirectory(connection);

	std::cout << "local_profile_test: all checks passed\n";
	return 0;
}
