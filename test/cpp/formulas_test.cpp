// Formulas over HTTPS (TASKS.md C8): /v1/catalog, /v1/batch for OF.TABLE,
// OF.CALL and OF.ASOF, the server's result cache, Local queries evaluated a
// group at a time, /v1/missing and ofexcel_call. A live server started by
// serve's own start sequence, with ofquack's transport scripted: no Fusion.

#include "base64.h"
#include "daemon/startup.hpp"
#include "ofexcel/formulas.hpp"
#include "ofexcel/hashing.hpp"
#include "ofexcel/json_body.hpp"
#include "ofexcel/local_ca.hpp"
#include "ofexcel/paths.hpp"
#include "ofexcel/run_engine.hpp"
#include "ofquack/errors.hpp"
#include "ofquack/transport.hpp"
#include "temporary_directory.hpp"
#include "test_support.hpp"

#include <httplib.h>

#include <cmath>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

using test_support::Contains;
using test_support::RunOk;

namespace fs = std::filesystem;

namespace {

const char *const ORIGIN = "https://app.example.com";
const char *const WORKBOOK = "0b6f3c1e-2a4d-4c8e-9f10-7d2e5a6b8c9d";

std::string ca_cert;

// --- The fake BI Publisher ------------------------------------------------------

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

bool AsksForALaterPage(const std::string &sql) {
	const auto at = sql.find("OFFSET ");
	return at != std::string::npos && sql.compare(at + 7, 1, "0") != 0;
}

std::mutex script_mutex;
std::string script_page;

void Answer(std::string page) {
	std::lock_guard<std::mutex> lock(script_mutex);
	script_page = std::move(page);
}

class FakeTransport : public ofquack::FusionTransport {
public:
	std::string Execute(const std::string &sql, const ofquack::RequestContext &) override {
		std::lock_guard<std::mutex> lock(script_mutex);
		return AsksForALaterPage(sql) ? EmptyPage() : script_page;
	}
};

std::vector<Row> InvoiceRows(const std::string &note = "first") {
	return {{{"INVOICE_ID", "101"}, {"AMT", ".84"}, {"NOTE", note}, {"DAY", "2026-01-01"}},
	        {{"INVOICE_ID", "102"}, {"AMT", "-1.5"}, {"DAY", "2026-02-01"}}};
}

// --- The daemon and its API -----------------------------------------------------

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

std::string JsonString(const std::string &text) {
	std::string quoted = "\"";
	for (const auto c : text) {
		if (c == '"' || c == '\\') {
			quoted += '\\';
			quoted += c;
		} else if (c == '\n') {
			quoted += "\\n";
		} else {
			quoted += c;
		}
	}
	return quoted + "\"";
}

int PortOf(const std::string &url) {
	return std::stoi(url.substr(url.rfind(':') + 1));
}

std::string directory_path;

//! A daemon whose init.sql has the fusion secret and `init`.
struct Daemon {
	ofexcel::PreparedDatabase prepared;
	std::unique_ptr<duckdb::Connection> connection;
	int port = 0;

	Daemon(const std::string &name, const std::string &init) {
		const auto home = ofexcel::JoinPath(directory_path, "home");
		WriteFile(ofexcel::JoinPath(home, ofexcel::INIT_FILE_NAME),
		          "SET ofexcel_projects_dir = " + SqlString(ofexcel::JoinPath(directory_path, name + "_projects")) +
		              ";\nCREATE SECRET fusion (TYPE oracle_fusion, "
		              "ENDPOINT 'https://fusion.example.com/xmlpserver/services/ExternalReportWSSService?WSDL', "
		              "REPORT_PATH '/Custom/Financials/RP_ARB.xdo', USERNAME 'analyst', PASSWORD 'hunter2');\n" +
		              init);
		ofexcel::StartupOptions options;
		options.db_path = ofexcel::JoinPath(directory_path, name + ".duckdb");
		options.port = "0";
		options.origin = ORIGIN;
		prepared = ofexcel::PrepareDaemonDatabase(options);
		connection = std::make_unique<duckdb::Connection>(*prepared.db);
		port = PortOf(RunOk(*connection, "SELECT url FROM ofexcel_start()").At(0, 0).ToString());
	}
	~Daemon() {
		RunOk(*connection, "SELECT stopped FROM ofexcel_stop()");
	}

	std::string Value(const std::string &sql) {
		const auto result = RunOk(*connection, sql);
		CHECK(result.rows.size() == 1);
		return result.At(0, 0).IsNull() ? "NULL" : result.At(0, 0).ToString();
	}
};

class Api {
public:
	explicit Api(int port) : client("127.0.0.1", port) {
		client.set_ca_cert_path(ca_cert.c_str());
		client.enable_server_certificate_verification(true);
		client.set_connection_timeout(5);
		client.set_read_timeout(60);
	}
	httplib::Result Get(const std::string &path) {
		return client.Get(path, Headers());
	}
	httplib::Result Post(const std::string &path, const std::string &body) {
		return client.Post(path, Headers(), body, "application/json");
	}
	httplib::Result Put(const std::string &path, const std::string &body) {
		return client.Put(path, Headers(), body, "application/json");
	}

private:
	static httplib::Headers Headers() {
		return {{"Origin", ORIGIN}, {"X-OF-Workbook", WORKBOOK}};
	}
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

std::string Query(const std::string &id, const std::string &extra, const std::string &body,
                  const std::string &engine = "oracle") {
	return "-- @id: " + id + "\n-- @engine: " + engine + "\n-- @version: 1\n" + extra + body + "\n";
}

const std::string INVOICES = Query("invoices", "-- @connection: fusion\n-- @param period: string\n",
                                   "SELECT invoice_id, amt, note, day FROM ap_invoices WHERE period = :period");

void Save(Api &api, const std::string &text) {
	CheckStatus(api.Put("/v1/queries/" + ofexcel::ParseQueryFile(text).header.id,
	                    "{\"project\":\"fin\",\"text\":" + JsonString(text) + "}"),
	            200);
}

std::string RunAndWait(Api &api, const std::string &text, const std::string &params) {
	const auto started = api.Post("/v1/runs", "{\"query\":{\"id\":\"" + ofexcel::ParseQueryFile(text).header.id +
	                                              "\",\"hash\":\"" + ofexcel::DefinitionHash(text) +
	                                              "\"},\"params\":" + params + "}");
	CheckStatus(started, 202);
	const auto document = ofexcel::JsonDocument::Parse(started->body);
	const auto run_id = document.Root().Get("run_id").String();
	ofexcel::RunEngine::Get().Wait(run_id);
	return run_id;
}

//! One call: {"i": i, "kind": kind, "query": query, "args": args}.
std::string Call(int i, const std::string &kind, const std::string &query, const std::string &args = "[]") {
	return "{\"i\":" + std::to_string(i) + ",\"kind\":\"" + kind + "\",\"query\":\"" + query + "\",\"args\":" + args +
	       "}";
}

//! The batch's results, as JSON text each.
std::vector<std::string> Batch(Api &api, const std::vector<std::string> &calls, const std::string &extra = "") {
	std::string body = "{\"calls\":[";
	for (size_t n = 0; n < calls.size(); n++) {
		body += (n ? "," : "") + calls[n];
	}
	const auto response = api.Post("/v1/batch", body + "]" + extra + "}");
	CheckStatus(response, 200);
	CHECK(Contains(response->body, "\"data_version\":"));
	CHECK(Contains(response->body, "\"catalog_version\":"));
	const auto document = ofexcel::JsonDocument::Parse(response->body);
	const auto results = document.Root().Get("results");
	CHECK(results.Size() == calls.size());
	std::vector<std::string> out;
	const auto start = response->body.find("\"results\":[") + 11;
	// Each result is an object at the top level of the array: split by depth.
	int depth = 0;
	size_t from = start;
	bool in_string = false;
	for (size_t at = start; at < response->body.size(); at++) {
		const char c = response->body[at];
		if (in_string) {
			if (c == '\\') {
				at++;
			} else if (c == '"') {
				in_string = false;
			}
			continue;
		}
		if (c == '"') {
			in_string = true;
		} else if (c == '{' || c == '[') {
			if (depth++ == 0) {
				from = at;
			}
		} else if (c == '}' || c == ']') {
			if (--depth == 0) {
				out.push_back(response->body.substr(from, at - from + 1));
			} else if (depth < 0) {
				break;
			}
		}
	}
	CHECK(out.size() == calls.size());
	return out;
}

void Expect(const std::string &result, const std::string &expected) {
	if (result != expected) {
		std::cerr << "expected " << expected << "\n     got " << result << "\n";
	}
	CHECK(result == expected);
}

void ExpectError(const std::string &result, const std::string &code, const std::string &message) {
	if (!Contains(result, "\"code\":\"" + code + "\"") || !Contains(result, message)) {
		std::cerr << "expected " << code << " " << message << "\n     got " << result << "\n";
	}
	CHECK(Contains(result, "\"e\":{\"code\":\"" + code + "\""));
	CHECK(Contains(result, message));
}

// --- Tests ------------------------------------------------------------------

//! §10.1: the core's four functions.
void TestCatalog(Daemon &daemon) {
	Api api(daemon.port);
	const auto catalog = api.Get("/v1/catalog");
	CheckStatus(catalog, 200);
	CHECK(Contains(catalog->body, "{\"catalog_version\":\""));
	CHECK(Contains(catalog->body, "{\"id\":\"TABLE\",\"name\":\"TABLE\",\"description\":\"QExcel query result as an "
	                              "array; the first row holds the headers\",\"params\":[{\"name\":\"query\","
	                              "\"type\":\"string\"},{\"name\":\"args\",\"type\":\"any\"}]}"));
	CHECK(Contains(catalog->body, "{\"id\":\"STATUS\",\"name\":\"STATUS\",\"description\":\"QExcel status: OK, "
	                              "NOT_SIGNED_IN, STALE, OFFLINE\",\"params\":[]}"));
	const auto document = ofexcel::JsonDocument::Parse(catalog->body);
	CHECK(document.Root().Get("functions").Size() == 4);
}

//! OF.TABLE over an Oracle query: missing data is #N/A and a meta.missing
//! record; once a run stored it, the table with headers, §11.4 values.
void TestOracleTable(Daemon &daemon) {
	Api api(daemon.port);
	Save(api, INVOICES);
	const auto table = Call(1, "table", "invoices", "[[[\"2026-01\"]]]");

	ExpectError(Batch(api, {table})[0], "notAvailable", "No data for invoices");
	auto missing = api.Get(std::string("/v1/missing?workbook=") + WORKBOOK);
	CheckStatus(missing, 200);
	CHECK(Contains(missing->body, "{\"missing\":[{\"query\":{\"id\":\"invoices\",\"hash\":\"" +
	                                  ofexcel::DefinitionHash(INVOICES) +
	                                  "\"},\"connection\":\"fusion\",\"params\":{\"period\":\"2026-01\"},"));
	CHECK(Contains(missing->body, "\"requests\":1}]}"));
	Batch(api, {table});
	CHECK(Contains(api.Get(std::string("/v1/missing?workbook=") + WORKBOOK)->body, "\"requests\":2}]}"));

	Answer(Page(InvoiceRows()));
	RunAndWait(api, INVOICES, "{\"period\":\"2026-01\"}");
	// The run filled what was missing.
	CHECK(api.Get(std::string("/v1/missing?workbook=") + WORKBOOK)->body == "{\"missing\":[]}");

	Expect(Batch(api, {table})[0], "{\"i\":1,\"v\":[[\"INVOICE_ID\",\"AMT\",\"NOTE\",\"DAY\"],"
	                               "[\"101\",0.84,\"first\",46023.0],[\"102\",-1.5,\"\",46054.0]]}");
	// The 1904 date system counts from 1904-01-01.
	Expect(Batch(api, {table}, ",\"date_system\":1904")[0],
	       "{\"i\":1,\"v\":[[\"INVOICE_ID\",\"AMT\",\"NOTE\",\"DAY\"],"
	       "[\"101\",0.84,\"first\",44561.0],[\"102\",-1.5,\"\",44592.0]]}");

	const auto results =
	    Batch(api, {Call(2, "call", "invoices", "[[[\"2026-01\"]]]"), Call(3, "asof", "invoices", "[[[\"2026-01\"]]]"),
	                Call(4, "table", "nothing"), Call(5, "table", "invoices", "[[[\"2026-01\"]],[[1]]]"),
	                Call(6, "table", "invoices", "[[[\"2026-01\"],[\"2026-02\"]]]"), Call(7, "table", "invoices"),
	                Call(8, "sum", "invoices")});
	ExpectError(results[0], "invalidValue", "Expected a 1 × 1 result, got 2 × 4");
	// ASOF: when it was fetched, in local time, as a serial number.
	const auto asof_document = ofexcel::JsonDocument::Parse(results[1]);
	const auto asof = asof_document.Root().Get("v");
	CHECK(asof.Is(ofexcel::JsonKind::NUMBER));
	const auto now_serial = 25569.0 + static_cast<double>(std::time(nullptr)) / 86400.0;
	CHECK(std::fabs(std::stod(asof.NumberText()) - now_serial) < 1.0);
	ExpectError(results[2], "invalidName", "Unknown query nothing");
	ExpectError(results[3], "invalidValue", "invoices takes 1 arguments, not 2");
	ExpectError(results[4], "invalidValue", "argument 1 (period) must be a single cell");
	ExpectError(results[5], "invalidValue", "parameter period");
	ExpectError(results[6], "invalidValue", "Unknown formula kind sum");
}

//! What a batch needs to find: a workbook's own query needs trust; an Oracle
//! query needs a connection, the workbook's or its own.
void TestResolution(Daemon &daemon) {
	Api api(daemon.port);
	const auto unbound = Query("unbound", "", "SELECT 1 AS one FROM dual");
	CheckStatus(api.Put(std::string("/v1/workbooks/") + WORKBOOK + "/queries",
	                    "{\"queries\":[{\"text\":" + JsonString(unbound) + "}]}"),
	            200);
	ExpectError(Batch(api, {Call(1, "table", "unbound")})[0], "notAvailable", "Query not trusted");
	CheckStatus(api.Post("/v1/trust", std::string("{\"workbook_id\":\"") + WORKBOOK + "\",\"hashes\":[\"" +
	                                      ofexcel::DefinitionHash(unbound) + "\"]}"),
	            200);
	ExpectError(Batch(api, {Call(1, "table", "unbound")})[0], "notAvailable", "No connection selected");
	ExpectError(Batch(api, {Call(1, "table", "unbound")}, ",\"connection\":\"fusion\"")[0], "notAvailable",
	            "No data for unbound");

	CheckStatus(api.Post("/v1/batch", "{\"calls\":{}}"), 422, "E_BAD_REQUEST");
	CheckStatus(api.Post("/v1/batch", "{\"calls\":[{\"i\":\"1\",\"kind\":\"table\",\"query\":\"q\"}]}"), 422,
	            "E_BAD_REQUEST");
	CheckStatus(api.Post("/v1/batch", "{\"calls\":[" + Call(1, "table", "q", "[[1]]") + "]}"), 422, "E_BAD_REQUEST");
	CheckStatus(api.Post("/v1/batch", "{\"calls\":[],\"date_system\":1905}"), 422, "E_BAD_REQUEST");
	CheckStatus(api.Get("/v1/missing?workbook=nope"), 422, "E_BAD_REQUEST");
}

//! The server's cache: the same call is answered from it until the
//! instance's data_version moves.
void TestCache(Daemon &daemon) {
	Api api(daemon.port);
	const auto table = Call(1, "table", "invoices", "[[[\"2026-01\"]]]");
	const auto before = Batch(api, {table})[0];
	CHECK(Contains(before, "\"first\""));
	// Behind the daemon's back: the cached result does not see it.
	const auto stored = daemon.Value("SELECT current_table FROM meta.dataset_instances WHERE query_id = 'invoices'");
	RunOk(*daemon.connection, "UPDATE ds.\"" + stored + "\" SET NOTE = 'edited'");
	Expect(Batch(api, {table})[0], before);

	Answer(Page(InvoiceRows("second")));
	RunAndWait(api, INVOICES, "{\"period\":\"2026-01\"}");
	CHECK(Contains(Batch(api, {table})[0], "\"second\""));
}

//! More rows than ofexcel_spill_max_rows (2 here) do not spill.
void TestSpill(Daemon &daemon) {
	Api api(daemon.port);
	auto rows = InvoiceRows();
	rows.push_back({{"INVOICE_ID", "103"}, {"AMT", "1"}, {"NOTE", "third"}, {"DAY", "2026-03-01"}});
	Answer(Page(rows));
	RunAndWait(api, INVOICES, "{\"period\":\"2026-03\"}");
	ExpectError(Batch(api, {Call(1, "table", "invoices", "[[[\"2026-03\"]]]")})[0], "notAvailable",
	            "Result too large: load it into a table");
}

//! Local queries in formulas: evaluated over their dependencies' current
//! data, a group per query; a data error stays in its own cell.
void TestLocal(Daemon &daemon) {
	Api api(daemon.port);
	const auto doubled = Query("doubled", "-- @param s: string\n", "SELECT CAST($s AS INTEGER) * 2 AS v", "local");
	const auto totals = Query("totals", "-- @param p: string\n-- @depends: invoices(period := $p)\n",
	                          "SELECT count(*) AS n, sum(amt) AS total FROM invoices", "local");
	Save(api, doubled);
	Save(api, totals);

	auto results = Batch(api, {Call(1, "call", "doubled", "[[[\"1\"]]]"), Call(2, "call", "doubled", "[[[\"x\"]]]"),
	                           Call(3, "call", "doubled", "[[[\"3\"]]]"), Call(4, "call", "doubled", "[[[1]]]")});
	Expect(results[0], "{\"i\":1,\"v\":2.0}");
	ExpectError(results[1], "invalidValue", "The query failed on its data");
	Expect(results[2], "{\"i\":3,\"v\":6.0}");
	// The same arguments: evaluated once, answered twice.
	Expect(results[3], "{\"i\":4,\"v\":2.0}");

	Answer(Page(InvoiceRows()));
	RunAndWait(api, INVOICES, "{\"period\":\"2026-04\"}");
	results = Batch(api, {Call(1, "table", "totals", "[[[\"2026-04\"]]]"),
	                      Call(2, "table", "totals", "[[[\"2026-05\"]]]")});
	Expect(results[0], "{\"i\":1,\"v\":[[\"n\",\"total\"],[2.0,-0.66]]}");
	// No data for the dependency: #N/A, and the workbook misses it.
	ExpectError(results[1], "notAvailable", "needs data that is not loaded: invoices");
	CHECK(Contains(api.Get(std::string("/v1/missing?workbook=") + WORKBOOK)->body,
	               "\"params\":{\"period\":\"2026-05\"}"));
	// ASOF of an evaluated Local query: its oldest dependency's.
	const auto asof = Batch(api, {Call(1, "asof", "totals", "[[[\"2026-04\"]]]")})[0];
	CHECK(Contains(asof, "{\"i\":1,\"v\":"));

	// ofexcel_call takes the same path.
	CHECK(daemon.Value("SELECT ofexcel_call('call', 'doubled', '[[[\"4\"]]]')") == "{\"v\":8.0}");
	CHECK(Contains(daemon.Value("SELECT ofexcel_call('table', 'nothing', NULL)"), "invalidName"));
	CHECK(daemon.Value("SELECT count(*) FROM ofexcel_missing() WHERE query_id = 'invoices'") != "0");

	// Records older than 7 days go when the list is read.
	RunOk(*daemon.connection, "UPDATE meta.missing SET last_seen = TIMESTAMP '2020-01-01 00:00:00'");
	CHECK(api.Get(std::string("/v1/missing?workbook=") + WORKBOOK)->body == "{\"missing\":[]}");
}

//! Out of memory is not a data error: the whole group gets it, and the rest of
//! the group is not evaluated -- even arguments that alone would work.
void TestOutOfMemory() {
	Daemon daemon("oom", "SET memory_limit = '64MB';\nSET temp_directory = '';\n");
	Api api(daemon.port);
	const auto distinct =
	    Query("distinct_count", "-- @param n: integer\n",
	          "SELECT count(*) AS c FROM (SELECT DISTINCT x::VARCHAR || 'padding-padding' AS y FROM range($n) t(x))",
	          "local");
	Save(api, distinct);
	const auto results = Batch(api, {Call(1, "call", "distinct_count", "[[[50000000]]]"),
	                                 Call(2, "call", "distinct_count", "[[[3]]]")});
	ExpectError(results[0], "notAvailable", "out of memory");
	ExpectError(results[1], "notAvailable", "out of memory");
	Expect(Batch(api, {Call(2, "call", "distinct_count", "[[[3]]]")})[0], "{\"i\":2,\"v\":3.0}");
}

} // namespace

int main() {
	test_support::TemporaryDirectory directory("ofexcel_formulas_test");
	directory_path = directory.path;
	const auto home = ofexcel::JoinPath(directory.path, "home");
	SetQexcelHome(home);
	const auto tls = ofexcel::TlsFilesIn(ofexcel::DefaultTlsDir());
	ofexcel::InstallLocalCa(tls.dir);
	ca_cert = tls.ca_cert;
	auto installed = ofquack::ScopedTransportFactory(
	    [](const ofquack::FusionConfig &) { return std::make_shared<FakeTransport>(); });

	{
		Daemon daemon("formulas", "SET ofexcel_spill_max_rows = 2;\n");
		TestCatalog(daemon);
		TestOracleTable(daemon);
		TestResolution(daemon);
		TestCache(daemon);
		TestSpill(daemon);
		TestLocal(daemon);
	}
	ofexcel::ClearFormulaCache();
	TestOutOfMemory();
	std::cout << "formulas_test: all checks passed\n";
	return 0;
}
