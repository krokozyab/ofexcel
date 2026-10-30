// Connections to Fusion and signing in to them (TASKS.md A4), against a live
// HTTPS server started by serve's own start sequence: meta.connections and the
// secrets beside it, the secrets recreated after a restart with init.sql's
// winning, /v1/connections/{name}/test through ofquack's transport seam, and
// sign-in with a token already cached, so no browser opens.

#include "base64.h"
#include "daemon/startup.hpp"
#include "ofexcel/local_ca.hpp"
#include "ofexcel/paths.hpp"
#include "ofquack/errors.hpp"
#include "ofquack/token_cache.hpp"
#include "ofquack/transport.hpp"
#include "temporary_directory.hpp"
#include "test_support.hpp"

#include <httplib.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

using test_support::Contains;
using test_support::Run;
using test_support::RunOk;

namespace fs = std::filesystem;

namespace {

const char *const ORIGIN = "https://app.example.com";
const char *const FUSION_HOST = "fa.example.com";

std::string ca_cert;

void SetQexcelHome(const std::string &path) {
#ifdef _WIN32
	CHECK(_putenv_s("QEXCEL_HOME", path.c_str()) == 0);
#else
	CHECK(setenv("QEXCEL_HOME", path.c_str(), 1) == 0);
#endif
}

void WriteFile(const std::string &path, const std::string &content) {
	fs::create_directories(fs::u8path(path).parent_path());
	std::ofstream file(fs::u8path(path), std::ios::binary);
	file << content;
	CHECK(file);
}

int PortOf(const std::string &url) {
	return std::stoi(url.substr(url.rfind(':') + 1));
}

//! What the hosted add-in sends: its Origin, JSON bodies, and patience enough
//! for a sign-in.
class Api {
public:
	explicit Api(int port) : client("127.0.0.1", port) {
		client.set_ca_cert_path(ca_cert.c_str());
		client.enable_server_certificate_verification(true);
		client.set_connection_timeout(5);
	}

	httplib::Result Get(const std::string &path) {
		return client.Get(path, Headers());
	}
	httplib::Result Post(const std::string &path, const std::string &body) {
		return client.Post(path, Headers(), body, "application/json");
	}

private:
	static httplib::Headers Headers() {
		return {{"Origin", ORIGIN}};
	}
	httplib::SSLClient client;
};

void CheckStatus(const httplib::Result &response, int status, const std::string &code = "") {
	CHECK(response);
	if (response->status != status) {
		std::cerr << "expected " << status << ", got " << response->status << ": " << response->body << "\n";
	}
	CHECK(response->status == status);
	CHECK(response->has_header("X-OF-Data-Version"));
	if (!code.empty()) {
		CHECK(Contains(response->body, "\"code\":\"" + code + "\""));
	}
}

//! The daemon as `ofexceld serve` prepares it, then started on a free port.
struct Daemon {
	ofexcel::PreparedDatabase prepared;
	std::unique_ptr<duckdb::Connection> connection;
	int port = 0;

	explicit Daemon(const std::string &db_path) {
		ofexcel::StartupOptions options;
		options.db_path = db_path;
		options.port = "0";
		options.origin = ORIGIN;
		prepared = ofexcel::PrepareDaemonDatabase(options);
		connection = std::make_unique<duckdb::Connection>(*prepared.db);
		port = PortOf(RunOk(*connection, "SELECT url FROM ofexcel_start()").At(0, 0).ToString());
	}
	~Daemon() {
		RunOk(*connection, "SELECT stopped FROM ofexcel_stop()");
	}

	//! duckdb_secrets() as "name|provider|secret_string": what the secret
	//! manager holds, with the redaction it applies.
	std::string Secret(const std::string &name) {
		const auto rows = RunOk(*connection, "SELECT provider, secret_string FROM duckdb_secrets() WHERE name = '" +
		                                         name + "'");
		if (rows.rows.empty()) {
			return "";
		}
		return name + "|" + rows.At(0, 0).ToString() + "|" + rows.At(0, 1).ToString();
	}
};

std::string ConnectionBody(const std::string &name, const std::string &environment, const std::string &endpoint,
                           const std::string &report_path = "/Custom/Financials/RP_ARB.xdo") {
	return "{\"name\":\"" + name + "\",\"environment\":\"" + environment + "\",\"endpoint\":\"" + endpoint +
	       "\",\"report_path\":\"" + report_path + "\"}";
}

// --- Fusion behind ofquack's transport seam -----------------------------------

std::string MakeSoapResponse(const std::string &report_xml) {
	const auto encoded = base64_encode(reinterpret_cast<const unsigned char *>(report_xml.c_str()), report_xml.size());
	return "<soap:Envelope xmlns:soap=\"http://www.w3.org/2003/05/soap-envelope\"><soap:Body>"
	       "<ns2:runReportResponse><ns2:runReportReturn><ns2:reportBytes>" +
	       encoded + "</ns2:reportBytes></ns2:runReportReturn></ns2:runReportResponse></soap:Body></soap:Envelope>";
}

std::string MakeReportXML(const std::string &escaped_rowset) {
	return "<DATA_DS><G_1><RESULT>" + escaped_rowset + "</RESULT></G_1></DATA_DS>";
}

enum class FusionAnswer { ONE_ROW, AUTHENTICATION_ERROR };

class FakeFusion : public ofquack::FusionTransport {
public:
	explicit FakeFusion(FusionAnswer answer_p) : answer(answer_p) {
	}

	std::string Execute(const std::string &sql, const ofquack::RequestContext &) override {
		if (answer == FusionAnswer::AUTHENTICATION_ERROR) {
			throw ofquack::AuthenticationError("Oracle Fusion rejected the credentials (HTTP 401)");
		}
		// A scan ends on an empty page.
		const auto offset = sql.find("OFFSET ");
		if (offset != std::string::npos && sql.compare(offset + 7, 1, "0") != 0) {
			return MakeSoapResponse(MakeReportXML("&lt;ROWSET/&gt;"));
		}
		return MakeSoapResponse(
		    MakeReportXML("&lt;ROWSET&gt;&lt;ROW&gt;&lt;X&gt;1&lt;/X&gt;&lt;/ROW&gt;&lt;/ROWSET&gt;"));
	}

private:
	FusionAnswer answer;
};

ofquack::ScopedTransportFactory InstallFusion(FusionAnswer answer) {
	return ofquack::ScopedTransportFactory(
	    [answer](const ofquack::FusionConfig &) { return std::make_shared<FakeFusion>(answer); });
}

//! An unsigned JWT for `subject`, valid for an hour: what the browser sign-in
//! would have left in ofquack's token cache.
std::string FakeJwt(const std::string &subject) {
	const auto now = std::chrono::duration_cast<std::chrono::seconds>(
	                     std::chrono::system_clock::now().time_since_epoch())
	                     .count();
	const auto part = [](const std::string &json) {
		auto encoded = base64_encode(json, true);
		while (!encoded.empty() && (encoded.back() == '=' || encoded.back() == '.')) {
			encoded.pop_back();
		}
		return encoded;
	};
	return part("{\"alg\":\"none\"}") + "." +
	       part("{\"sub\":\"" + subject + "\",\"exp\":" + std::to_string(now + 3600) + "}") + ".signature";
}

// --- Tests ---------------------------------------------------------------------

void TestCreateConnection(Daemon &daemon) {
	Api api(daemon.port);
	const auto created =
	    api.Post("/v1/connections", ConnectionBody("fusion_test", "TEST", "HTTPS://FA.Example.com"));
	CheckStatus(created, 201);
	// The endpoint comes back as stored: lower case, no path.
	CHECK(created->body == "{\"name\":\"fusion_test\",\"environment\":\"TEST\",\"endpoint\":\"https://fa.example.com\","
	                       "\"report_path\":\"/Custom/Financials/RP_ARB.xdo\"}");
	CHECK(daemon.Secret("fusion_test") ==
	      "fusion_test|browser|name=fusion_test;type=oracle_fusion;provider=browser;serializable=true;scope;"
	      "endpoint=https://fa.example.com;report_path=/Custom/Financials/RP_ARB.xdo;sso_timeout_seconds=300");

	CheckStatus(api.Post("/v1/connections", ConnectionBody("fusion_dev", "DEV", "https://fa-dev.example.com:8443")),
	            201);
}

void TestNameTaken(Daemon &daemon) {
	Api api(daemon.port);
	CheckStatus(api.Post("/v1/connections", ConnectionBody("fusion_test", "PROD", "https://other.example.com")), 409,
	            "E_CONFLICT");
	// A secret init.sql or SQL created holds the name as well.
	RunOk(*daemon.connection, "CREATE SECRET by_hand (TYPE oracle_fusion, PROVIDER browser, "
	                          "ENDPOINT 'https://hand.example.com', REPORT_PATH '/x.xdo')");
	CheckStatus(api.Post("/v1/connections", ConnectionBody("by_hand", "DEV", "https://hand.example.com")), 409,
	            "E_CONFLICT");
	// The refused request changed nothing.
	CHECK(Contains(daemon.Secret("fusion_test"), "endpoint=https://fa.example.com;"));
}

void TestBadDefinitions(Daemon &daemon) {
	Api api(daemon.port);
	CheckStatus(api.Post("/v1/connections", ConnectionBody("fusion_x", "STAGE", "https://fa.example.com")), 422,
	            "E_BAD_REQUEST");
	CheckStatus(api.Post("/v1/connections", ConnectionBody("fusion_x", "test", "https://fa.example.com")), 422,
	            "E_BAD_REQUEST");
	CheckStatus(api.Post("/v1/connections", ConnectionBody("Fusion", "DEV", "https://fa.example.com")), 422,
	            "E_BAD_REQUEST");
	for (const auto *endpoint : {"http://fa.example.com", "https://fa.example.com/xmlpserver",
	                             "https://fa.example.com/", "https://user@fa.example.com", "fa.example.com", ""}) {
		CheckStatus(api.Post("/v1/connections", ConnectionBody("fusion_x", "DEV", endpoint)), 422, "E_BAD_REQUEST");
	}
	CheckStatus(api.Post("/v1/connections", ConnectionBody("fusion_x", "DEV", "https://fa.example.com", "x.xdo")), 422,
	            "E_BAD_REQUEST");
	CheckStatus(api.Post("/v1/connections", "{\"name\":\"fusion_x\",\"environment\":\"DEV\"}"), 422, "E_BAD_REQUEST");
	CheckStatus(api.Post("/v1/connections", "{\"name\":\"fusion_x\",\"environment\":\"DEV\",\"endpoint\":1,"
	                                        "\"report_path\":\"/x.xdo\"}"),
	            422, "E_BAD_REQUEST");
	CheckStatus(api.Post("/v1/connections", "not json"), 400, "E_BAD_REQUEST");
	CheckStatus(api.Post("/v1/connections", "[]"), 400, "E_BAD_REQUEST");
	CHECK(daemon.Secret("fusion_x").empty());
	CHECK(RunOk(*daemon.connection, "SELECT count(*) FROM meta.connections WHERE name = 'fusion_x'").At(0, 0) ==
	      duckdb::Value::BIGINT(0));
}

void TestListHasNoSecrets(Daemon &daemon) {
	const auto list = Api(daemon.port).Get("/v1/connections");
	CheckStatus(list, 200);
	CHECK(list->body ==
	      "{\"connections\":["
	      "{\"name\":\"fusion_dev\",\"environment\":\"DEV\",\"endpoint\":\"https://fa-dev.example.com:8443\","
	      "\"report_path\":\"/Custom/Financials/RP_ARB.xdo\"},"
	      "{\"name\":\"fusion_test\",\"environment\":\"TEST\",\"endpoint\":\"https://fa.example.com\","
	      "\"report_path\":\"/Custom/Financials/RP_ARB.xdo\"}]}");
}

void TestStatusBeforeSignIn(Daemon &daemon) {
	const auto status = Api(daemon.port).Get("/v1/status");
	CheckStatus(status, 200);
	CHECK(status->body == "{\"connections\":["
	                      "{\"name\":\"fusion_dev\",\"environment\":\"DEV\",\"host\":\"fa-dev.example.com:8443\","
	                      "\"signed_in\":false},"
	                      "{\"name\":\"fusion_test\",\"environment\":\"TEST\",\"host\":\"fa.example.com\","
	                      "\"signed_in\":false}],"
	                      "\"active_runs\":0,\"warnings\":[]}");
}

void TestConnectionTest(Daemon &daemon) {
	Api api(daemon.port);
	CheckStatus(api.Post("/v1/connections/nope/test", ""), 404, "E_NOT_FOUND");
	CheckStatus(api.Post("/v1/connections/NOPE/test", ""), 404, "E_NOT_FOUND");

	{
		// Not signed in: ofquack refuses before anything is sent.
		auto installed = InstallFusion(FusionAnswer::ONE_ROW);
		const auto response = api.Post("/v1/connections/fusion_test/test", "");
		CheckStatus(response, 200);
		CHECK(Contains(response->body, "\"ok\":false"));
		CHECK(Contains(response->body, "\"code\":\"E_FUSION_AUTH\""));
	}

	ofquack::TokenCache::Get().Store(FUSION_HOST, FakeJwt("analyst@example.com"), "", 3600);
	{
		auto installed = InstallFusion(FusionAnswer::ONE_ROW);
		const auto response = api.Post("/v1/connections/fusion_test/test", "");
		CheckStatus(response, 200);
		CHECK(response->body == "{\"ok\":true,\"user\":\"analyst@example.com\"}");
	}
	{
		auto installed = InstallFusion(FusionAnswer::AUTHENTICATION_ERROR);
		const auto response = api.Post("/v1/connections/fusion_test/test", "");
		CheckStatus(response, 200);
		CHECK(Contains(response->body, "\"ok\":false"));
		CHECK(Contains(response->body, "\"error\":{\"code\":\"E_FUSION_AUTH\",\"message\":\""));
	}
}

void TestStatusAfterSignIn(Daemon &daemon) {
	const auto status = Api(daemon.port).Get("/v1/status");
	CheckStatus(status, 200);
	CHECK(Contains(status->body, "{\"name\":\"fusion_test\",\"environment\":\"TEST\",\"host\":\"fa.example.com\","
	                             "\"signed_in\":true,\"user\":\"analyst@example.com\"}"));
	CHECK(!Contains(status->body, "signature"));
}

void TestLoginAndLogout(Daemon &daemon) {
	Api api(daemon.port);
	// The token from TestConnectionTest is still valid, so ofquack hands it back
	// without opening a browser.
	const auto login = api.Post("/v1/auth/login", "{\"connection\":\"fusion_test\"}");
	CheckStatus(login, 200);
	CHECK(login->body == "{\"user\":\"analyst@example.com\"}");

	CheckStatus(api.Post("/v1/auth/login", "{\"connection\":\"nope\"}"), 404, "E_NOT_FOUND");
	CheckStatus(api.Post("/v1/auth/login", "{}"), 422, "E_BAD_REQUEST");
	CheckStatus(api.Post("/v1/auth/login", ""), 400, "E_BAD_REQUEST");
	CheckStatus(api.Post("/v1/auth/logout", "{\"connection\":\"nope\"}"), 404, "E_NOT_FOUND");

	CheckStatus(api.Post("/v1/auth/logout", "{\"connection\":\"fusion_test\"}"), 204);
	CHECK(!ofquack::TokenCache::Get().Lookup(FUSION_HOST).Valid());
	const auto status = api.Get("/v1/status");
	CheckStatus(status, 200);
	CHECK(Contains(status->body, "\"host\":\"fa.example.com\",\"signed_in\":false}"));
}

void TestSetEnvironment(Daemon &daemon) {
	auto &connection = *daemon.connection;
	const auto labelled = RunOk(connection, "SELECT * FROM ofexcel_set_environment('fusion_test', 'PROD')");
	CHECK(labelled.rows.size() == 1);
	CHECK(labelled.At(0, 0).ToString() == "fusion_test");
	CHECK(labelled.At(0, 1).ToString() == "PROD");
	CHECK(Contains(Api(daemon.port).Get("/v1/connections")->body,
	               "{\"name\":\"fusion_test\",\"environment\":\"PROD\""));

	// A secret without a row gets one, from the secret's own definition.
	RunOk(connection, "CREATE SECRET with_path (TYPE oracle_fusion, PROVIDER browser, "
	                  "ENDPOINT 'https://Path.Example.com/xmlpserver/services/ExternalReportWSSService?WSDL', "
	                  "REPORT_PATH '/Custom/X.xdo')");
	RunOk(connection, "SELECT * FROM ofexcel_set_environment('with_path', 'DEV')");
	CHECK(Contains(Api(daemon.port).Get("/v1/connections")->body,
	               "{\"name\":\"with_path\",\"environment\":\"DEV\",\"endpoint\":\"https://path.example.com\","
	               "\"report_path\":\"/Custom/X.xdo\"}"));

	auto refused = Run(connection, "SELECT * FROM ofexcel_set_environment('fusion_test', 'STAGE')");
	CHECK(Contains(refused.error, "E_BAD_REQUEST"));
	refused = Run(connection, "SELECT * FROM ofexcel_set_environment('nope', 'DEV')");
	CHECK(Contains(refused.error, "E_NOT_FOUND"));
	refused = Run(connection, "SELECT * FROM ofexcel_set_environment(NULL, 'DEV')");
	CHECK(Contains(refused.error, "E_BAD_REQUEST"));
}

//! Secrets live in memory. After a restart they come back from meta.connections,
//! except where init.sql created one under the same name: that one wins.
void TestRestartRecreatesSecrets(const std::string &home, const std::string &db_path) {
	WriteFile(ofexcel::JoinPath(home, ofexcel::INIT_FILE_NAME),
	          "CREATE SECRET fusion_test (TYPE oracle_fusion, PROVIDER browser, "
	          "ENDPOINT 'https://init.example.com', REPORT_PATH '/Init/R.xdo');\n"
	          "CREATE SECRET from_init (TYPE oracle_fusion, PROVIDER browser, "
	          "ENDPOINT 'https://init2.example.com', REPORT_PATH '/Init/R2.xdo');\n"
	          "SELECT * FROM ofexcel_set_environment('from_init', 'TEST');\n");
	Daemon daemon(db_path);
	CHECK(daemon.Secret("fusion_dev") ==
	      "fusion_dev|browser|name=fusion_dev;type=oracle_fusion;provider=browser;serializable=true;scope;"
	      "endpoint=https://fa-dev.example.com:8443;report_path=/Custom/Financials/RP_ARB.xdo;sso_timeout_seconds=300");
	CHECK(daemon.Secret("with_path") ==
	      "with_path|browser|name=with_path;type=oracle_fusion;provider=browser;serializable=true;scope;"
	      "endpoint=https://path.example.com;report_path=/Custom/X.xdo;sso_timeout_seconds=300");
	CHECK(Contains(daemon.Secret("fusion_test"), "endpoint=https://init.example.com;report_path=/Init/R.xdo"));
	// Only meta.connections and init.sql make secrets; one made by hand is gone.
	CHECK(daemon.Secret("by_hand").empty());

	const auto list = Api(daemon.port).Get("/v1/connections");
	CheckStatus(list, 200);
	CHECK(Contains(list->body, "{\"name\":\"from_init\",\"environment\":\"TEST\","
	                           "\"endpoint\":\"https://init2.example.com\",\"report_path\":\"/Init/R2.xdo\"}"));
	// The row keeps what was created over HTTP; the secret in use is init.sql's.
	CHECK(Contains(list->body,
	               "{\"name\":\"fusion_test\",\"environment\":\"PROD\",\"endpoint\":\"https://fa.example.com\""));
	CheckStatus(Api(daemon.port).Post("/v1/connections", ConnectionBody("from_init", "DEV", "https://x.example.com")),
	            409, "E_CONFLICT");
}

} // namespace

int main() {
	test_support::TemporaryDirectory directory("ofexcel_connections_test");
	const auto home = ofexcel::JoinPath(directory.path, "home");
	// Never the user's own QEXCEL_HOME: its init.sql would run here.
	SetQexcelHome(home);
	const auto tls = ofexcel::TlsFilesIn(ofexcel::DefaultTlsDir());
	ofexcel::InstallLocalCa(tls.dir);
	ca_cert = tls.ca_cert;
	const auto db_path = ofexcel::JoinPath(directory.path, "data.duckdb");

	{
		Daemon daemon(db_path);
		TestCreateConnection(daemon);
		TestNameTaken(daemon);
		TestBadDefinitions(daemon);
		TestListHasNoSecrets(daemon);
		TestStatusBeforeSignIn(daemon);
		TestConnectionTest(daemon);
		TestStatusAfterSignIn(daemon);
		TestLoginAndLogout(daemon);
		TestSetEnvironment(daemon);
	}
	TestRestartRecreatesSecrets(home, db_path);

	std::cout << "connections_test: all checks passed\n";
	return 0;
}
