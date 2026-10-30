// Fusion's dictionary over HTTPS (TASKS.md C9): tables filtered by a
// substring without regard to case, limit, a table's columns, and the errors.
// ofquack's transport answers its dictionary queries from a script; its
// metadata cache lives under a temporary HOME, never the user's own.

#include "base64.h"
#include "daemon/startup.hpp"
#include "ofexcel/local_ca.hpp"
#include "ofexcel/paths.hpp"
#include "ofquack/transport.hpp"
#include "temporary_directory.hpp"
#include "test_support.hpp"

#include <httplib.h>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using test_support::Contains;
using test_support::RunOk;

namespace fs = std::filesystem;

namespace {

const char *const ORIGIN = "https://app.example.com";

std::string ca_cert;

// --- The fake dictionary -----------------------------------------------------------

std::string MakeSoapResponse(const std::string &report_xml) {
	const auto encoded = base64_encode(reinterpret_cast<const unsigned char *>(report_xml.c_str()), report_xml.size());
	return "<soap:Envelope xmlns:soap=\"http://www.w3.org/2003/05/soap-envelope\"><soap:Body>"
	       "<ns2:runReportResponse><ns2:runReportReturn><ns2:reportBytes>" +
	       encoded + "</ns2:reportBytes></ns2:runReportReturn></ns2:runReportResponse></soap:Body></soap:Envelope>";
}

using Row = std::vector<std::pair<std::string, std::string>>;

std::string Page(const std::vector<Row> &rows) {
	std::string rowset = "&lt;ROWSET&gt;";
	for (const auto &row : rows) {
		rowset += "&lt;ROW&gt;";
		for (const auto &cell : row) {
			rowset += "&lt;" + cell.first + "&gt;" + cell.second + "&lt;/" + cell.first + "&gt;";
		}
		rowset += "&lt;/ROW&gt;";
	}
	return MakeSoapResponse("<DATA_DS><G_1><RESULT>" + rowset + "&lt;/ROWSET&gt;</RESULT></G_1></DATA_DS>");
}

std::string EmptyPage() {
	return MakeSoapResponse("<DATA_DS><G_1><RESULT>&lt;ROWSET/&gt;</RESULT></G_1></DATA_DS>");
}

Row Table(const std::string &name, const std::string &type, const std::string &id, const std::string &remarks) {
	return {{"TABLE_NAME", name}, {"TABLE_TYPE", type}, {"REMARKS", remarks}, {"TABLE_ID", id}};
}

Row Column(const std::string &name, const std::string &type, const std::string &precision, const std::string &scale,
           const std::string &ordinal, const std::string &remarks) {
	return {{"TABLE_NAME", "AP_INVOICES_ALL"}, {"COLUMN_NAME", name},       {"TYPE_NAME", type},
	        {"DECIMAL_DIGITS", precision},     {"NUM_PREC_RADIX", scale},  {"NULLABLE", "1"},
	        {"ORDINAL_POSITION", ordinal},     {"REMARKS", remarks},       {"TABLE_ID", "10"}};
}

std::atomic<int> requests {0};

//! Answers the dictionary queries ofquack sends (its metadata_queries.cpp): the
//! table count, the table list (one page, then the empty page after a seek),
//! the columns of a table by its FND id, and of a view.
class FakeTransport : public ofquack::FusionTransport {
public:
	std::string Execute(const std::string &sql, const ofquack::RequestContext &) override {
		requests++;
		if (sql.find("TABLE_COUNT") != std::string::npos) {
			return Page({{{"TABLE_COUNT", "5"}}});
		}
		if (sql.find("FROM FND_VIEWS") != std::string::npos) {
			if (sql.find("t.table_name > '") != std::string::npos) {
				return EmptyPage();
			}
			return Page({Table("AP_INVOICES_ALL", "TABLE", "10", "Supplier invoices"),
			             Table("AP_INVOICE_LINES_V", "VIEW", "", ""), Table("AP_SUPPLIERS", "TABLE", "11", "Suppliers"),
			             Table("GL_BALANCES", "TABLE", "12", "Balances"), Table("ZX_AP_TAX", "TABLE", "13", "")});
		}
		const auto offset = sql.find("OFFSET ");
		if (offset != std::string::npos && sql.compare(offset + 7, 1, "0") != 0) {
			return EmptyPage();
		}
		if (sql.find("FROM FND_COLUMNS") != std::string::npos) {
			return Page({Column("INVOICE_ID", "NUMBER", "18", "0", "1", "Invoice id"),
			             Column("INVOICE_AMOUNT", "NUMBER", "", "", "2", ""),
			             Column("INVOICE_NUM", "VARCHAR2", "", "", "3", "Invoice number")});
		}
		if (sql.find("all_tab_columns") != std::string::npos) {
			return Page({{{"TABLE_NAME", "AP_INVOICE_LINES_V"},
			              {"COLUMN_NAME", "LINE_NUMBER"},
			              {"TYPE_NAME", "NUMBER"},
			              {"ORDINAL_POSITION", "1"}}});
		}
		return EmptyPage();
	}
};

// --- The daemon and its API -----------------------------------------------------

void SetEnv(const char *name, const std::string &value) {
#ifdef _WIN32
	CHECK(_putenv_s(name, value.c_str()) == 0);
#else
	CHECK(setenv(name, value.c_str(), 1) == 0);
#endif
}

void WriteFile(const std::string &path, const std::string &content) {
	fs::create_directories(fs::u8path(path).parent_path());
	std::ofstream file(fs::u8path(path), std::ios::binary);
	file << content;
	CHECK(file);
}

std::string SqlString(const std::string &text) {
	std::string quoted = "'";
	for (const auto c : text) {
		quoted += c;
		if (c == '\'') {
			quoted += '\'';
		}
	}
	return quoted + "'";
}

int PortOf(const std::string &url) {
	return std::stoi(url.substr(url.rfind(':') + 1));
}

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
};

class Api {
public:
	explicit Api(int port) : client("127.0.0.1", port) {
		client.set_ca_cert_path(ca_cert.c_str());
		client.enable_server_certificate_verification(true);
		client.set_connection_timeout(5);
		client.set_read_timeout(30);
	}
	httplib::Result Get(const std::string &path) {
		return client.Get(path, httplib::Headers {{"Origin", ORIGIN}});
	}

private:
	httplib::SSLClient client;
};

void CheckStatus(const httplib::Result &response, int status, const std::string &code = "") {
	CHECK(response);
	if (response->status != status || (!code.empty() && !Contains(response->body, "\"code\":\"" + code + "\""))) {
		std::cerr << "expected " << status << " " << code << ", got " << response->status << ": " << response->body
		          << "\n";
	}
	CHECK(response->status == status);
	if (!code.empty()) {
		CHECK(Contains(response->body, "\"code\":\"" + code + "\""));
	}
}

void ExpectBody(Api &api, const std::string &path, const std::string &expected) {
	const auto response = api.Get(path);
	CheckStatus(response, 200);
	if (response->body != expected) {
		std::cerr << path << "\nexpected " << expected << "\n     got " << response->body << "\n";
	}
	CHECK(response->body == expected);
}

// --- Tests ------------------------------------------------------------------

//! A substring of the name without regard to case, names that start with it
//! first; limit; the list fetched once and then served from ofquack's cache.
void TestTables(Daemon &daemon) {
	Api api(daemon.port);
	ExpectBody(api, "/v1/dictionary/tables?connection=fusion&q=ap",
	           "{\"tables\":[{\"name\":\"AP_INVOICES_ALL\",\"description\":\"Supplier invoices\"},"
	           "{\"name\":\"AP_INVOICE_LINES_V\"},{\"name\":\"AP_SUPPLIERS\",\"description\":\"Suppliers\"},"
	           "{\"name\":\"ZX_AP_TAX\"}]}");
	const auto fetched = requests.load();
	CHECK(fetched > 0);
	ExpectBody(api, "/v1/dictionary/tables?connection=fusion&q=Invoice",
	           "{\"tables\":[{\"name\":\"AP_INVOICES_ALL\",\"description\":\"Supplier invoices\"},"
	           "{\"name\":\"AP_INVOICE_LINES_V\"}]}");
	ExpectBody(api, "/v1/dictionary/tables?connection=fusion&q=ap&limit=2",
	           "{\"tables\":[{\"name\":\"AP_INVOICES_ALL\",\"description\":\"Supplier invoices\"},"
	           "{\"name\":\"AP_INVOICE_LINES_V\"}]}");
	ExpectBody(api, "/v1/dictionary/tables?connection=fusion&q=nothing", "{\"tables\":[]}");
	const auto all = api.Get("/v1/dictionary/tables?connection=fusion");
	CheckStatus(all, 200);
	CHECK(Contains(all->body, "{\"name\":\"GL_BALANCES\",\"description\":\"Balances\"}"));
	// From ofquack's cache: no BI Publisher call after the first listing.
	CHECK(requests.load() == fetched);

	for (const auto *limit : {"0", "1001", "x", "-1"}) {
		CheckStatus(api.Get(std::string("/v1/dictionary/tables?connection=fusion&limit=") + limit), 422,
		            "E_BAD_REQUEST");
	}
	CheckStatus(api.Get("/v1/dictionary/tables?q=ap"), 422, "E_BAD_REQUEST");
	CheckStatus(api.Get("/v1/dictionary/tables?connection=nowhere"), 404, "E_NOT_FOUND");
}

//! A table's columns in order, with the Oracle type; a view's from
//! all_tab_columns; an unknown table is 404.
void TestColumns(Daemon &daemon) {
	Api api(daemon.port);
	ExpectBody(api, "/v1/dictionary/columns?connection=fusion&table=ap_invoices_all",
	           "{\"columns\":[{\"name\":\"INVOICE_ID\",\"type\":\"NUMBER(18,0)\",\"description\":\"Invoice id\"},"
	           "{\"name\":\"INVOICE_AMOUNT\",\"type\":\"NUMBER\"},"
	           "{\"name\":\"INVOICE_NUM\",\"type\":\"VARCHAR2\",\"description\":\"Invoice number\"}]}");
	ExpectBody(api, "/v1/dictionary/columns?connection=fusion&table=AP_INVOICE_LINES_V",
	           "{\"columns\":[{\"name\":\"LINE_NUMBER\",\"type\":\"NUMBER\"}]}");
	CheckStatus(api.Get("/v1/dictionary/columns?connection=fusion&table=NO_SUCH_TABLE"), 404, "E_NOT_FOUND");
	CheckStatus(api.Get("/v1/dictionary/columns?connection=fusion"), 422, "E_BAD_REQUEST");
	CheckStatus(api.Get("/v1/dictionary/columns?connection=nowhere&table=AP_INVOICES_ALL"), 404, "E_NOT_FOUND");
}

} // namespace

int main() {
	test_support::TemporaryDirectory directory("ofexcel_dictionary_test");
	// ofquack keeps its metadata cache under the home directory: a temporary one
	// here, before anything opens it.
	const auto user_home = ofexcel::JoinPath(directory.path, "user");
	fs::create_directories(fs::u8path(user_home));
	SetEnv("HOME", user_home);
	SetEnv("USERPROFILE", user_home);
	const auto home = ofexcel::JoinPath(directory.path, "home");
	SetEnv("QEXCEL_HOME", home);
	const auto tls = ofexcel::TlsFilesIn(ofexcel::DefaultTlsDir());
	ofexcel::InstallLocalCa(tls.dir);
	ca_cert = tls.ca_cert;
	WriteFile(ofexcel::JoinPath(home, ofexcel::INIT_FILE_NAME),
	          "SET ofexcel_projects_dir = " + SqlString(ofexcel::JoinPath(directory.path, "projects")) +
	              ";\nCREATE SECRET fusion (TYPE oracle_fusion, "
	              "ENDPOINT 'https://fusion.example.com/xmlpserver/services/ExternalReportWSSService?WSDL', "
	              "REPORT_PATH '/Custom/Financials/RP_ARB.xdo', USERNAME 'analyst', PASSWORD 'hunter2');\n");
	auto installed = ofquack::ScopedTransportFactory(
	    [](const ofquack::FusionConfig &) { return std::make_shared<FakeTransport>(); });
	{
		Daemon daemon(ofexcel::JoinPath(directory.path, "data.duckdb"));
		TestTables(daemon);
		TestColumns(daemon);
	}
	std::cout << "dictionary_test: all checks passed\n";
	return 0;
}
