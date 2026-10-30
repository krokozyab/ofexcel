// Trust (TASKS.md C2) and runs and datasets over HTTPS (TASKS.md C4), against a
// live server started by serve's own start sequence, with ofquack's transport
// replaced by a scripted fake: no Fusion, no network beyond 127.0.0.1.

#include "base64.h"
#include "daemon/startup.hpp"
#include "ofexcel/hashing.hpp"
#include "ofexcel/json_body.hpp"
#include "ofexcel/local_ca.hpp"
#include "ofexcel/paths.hpp"
#include "ofexcel/query_file.hpp"
#include "ofexcel/run_engine.hpp"
#include "ofquack/errors.hpp"
#include "ofquack/transport.hpp"
#include "temporary_directory.hpp"
#include "test_support.hpp"

#include <httplib.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
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

struct Script {
	std::mutex mutex;
	std::string first_page;
	bool hold = false;
	std::atomic<int> holding {0};
	std::vector<std::string> sql;

	void Answer(std::string page, bool hold_p = false) {
		std::lock_guard<std::mutex> lock(mutex);
		first_page = std::move(page);
		hold = hold_p;
		sql.clear();
	}
	std::string FirstSql() {
		std::lock_guard<std::mutex> lock(mutex);
		return sql.empty() ? "" : sql.front();
	}
};

Script script;

class FakeTransport : public ofquack::FusionTransport {
public:
	std::string Execute(const std::string &sql, const ofquack::RequestContext &context) override {
		std::string page;
		bool hold;
		{
			std::lock_guard<std::mutex> lock(script.mutex);
			script.sql.push_back(sql);
			page = script.first_page;
			hold = script.hold;
		}
		if (hold) {
			script.holding++;
			const auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds(60);
			while (!(context.is_cancelled && context.is_cancelled())) {
				CHECK(std::chrono::steady_clock::now() < give_up);
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
			}
			script.holding--;
			throw ofquack::CancelledError("cancelled");
		}
		return AsksForALaterPage(sql) ? EmptyPage() : page;
	}
};

std::vector<Row> InvoiceRows() {
	return {{{"INVOICE_ID", "101"}, {"AMT", ".84"}, {"NOTE", "first"}}, {{"INVOICE_ID", "102"}, {"AMT", "-1.5"}}};
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

	std::string Value(const std::string &sql) {
		const auto result = RunOk(*connection, sql);
		CHECK(result.rows.size() == 1);
		return result.At(0, 0).IsNull() ? "NULL" : result.At(0, 0).ToString();
	}
	bool TableExists(const std::string &table) {
		return Value("SELECT count(*) FROM duckdb_tables() WHERE schema_name = 'ds' AND table_name = " +
		             SqlString(table)) == "1";
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
		return client.Get(path, Headers());
	}
	httplib::Result Post(const std::string &path, const std::string &body) {
		return client.Post(path, Headers(), body, "application/json");
	}
	httplib::Result Put(const std::string &path, const std::string &body) {
		return client.Put(path, Headers(), body, "application/json");
	}
	httplib::Result Delete(const std::string &path) {
		return client.Delete(path, Headers());
	}

private:
	static httplib::Headers Headers() {
		return {{"Origin", ORIGIN}};
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
	CHECK(response->has_header("X-OF-Data-Version"));
	if (!code.empty()) {
		CHECK(Contains(response->body, "\"code\":\"" + code + "\""));
	}
}

//! A member of a JSON response body, as text: strings unquoted, numbers as
//! written, true/false; "" when missing.
std::string Field(const std::string &body, const std::string &path_1, const std::string &path_2 = "") {
	const auto document = ofexcel::JsonDocument::Parse(body);
	auto value = document.Root().Get(path_1);
	if (!path_2.empty()) {
		value = value.Get(path_2);
	}
	switch (value.Kind()) {
	case ofexcel::JsonKind::STRING:
		return value.String();
	case ofexcel::JsonKind::NUMBER:
		return value.NumberText();
	case ofexcel::JsonKind::BOOLEAN:
		return value.Bool() ? "true" : "false";
	case ofexcel::JsonKind::NULL_VALUE:
		return "null";
	default:
		return "";
	}
}

std::string Query(const std::string &id, const std::string &extra, const std::string &body) {
	return "-- @id: " + id + "\n-- @engine: oracle\n-- @version: 1\n-- @connection: fusion\n" + extra + body + "\n";
}

const std::string INVOICES = Query("invoices", "-- @param period: string = 2026-01\n-- @param bu: integer = 1\n",
                                   "SELECT invoice_id, amt, note FROM ap_invoices WHERE period = :period AND bu = :bu");

std::string RunBody(const std::string &text, const std::string &extra = "") {
	return "{\"query\":{\"id\":\"" + ofexcel::ParseQueryFile(text).header.id + "\",\"hash\":\"" +
	       ofexcel::DefinitionHash(text) + "\"}" + extra + "}";
}

//! Starts a run over HTTP and waits for it in process.
std::string RunAndWait(Api &api, const std::string &body) {
	const auto started = api.Post("/v1/runs", body);
	CheckStatus(started, 202);
	const auto run_id = Field(started->body, "run_id");
	ofexcel::RunEngine::Get().Wait(run_id);
	return run_id;
}

void WaitUntilHolding() {
	const auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds(30);
	while (script.holding.load() == 0) {
		CHECK(std::chrono::steady_clock::now() < give_up);
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
}

// --- C2: trust --------------------------------------------------------------

//! A workbook's query runs only once the user confirmed its hash; a changed
//! text asks again; only the workbook's own hashes can be confirmed.
void TestTrust(Daemon &daemon) {
	Api api(daemon.port);
	const auto text = Query("shared", "", "SELECT invoice_id, amt, note FROM ap_invoices");
	const auto hash = ofexcel::DefinitionHash(text);
	auto registered = api.Put(std::string("/v1/workbooks/") + WORKBOOK + "/queries",
	                          "{\"queries\":[{\"text\":" + JsonString(text) + "}]}");
	CheckStatus(registered, 200);
	CHECK(Contains(registered->body, "\"trusted\":false"));

	const auto runs_before = daemon.Value("SELECT count(*) FROM meta.runs");
	const auto body = RunBody(text, std::string(",\"workbook_id\":\"") + WORKBOOK + "\"");
	CheckStatus(api.Post("/v1/runs", body), 403, "E_NOT_TRUSTED");
	CHECK(daemon.Value("SELECT count(*) FROM meta.runs") == runs_before);
	CHECK(daemon.Value("SELECT count(*) FROM meta.dataset_instances WHERE query_id = 'shared'") == "0");

	// Only what the workbook holds can be confirmed, and nothing is trusted then.
	const auto stranger = ofexcel::DefinitionHash(Query("stranger", "", "SELECT 1 FROM dual"));
	CheckStatus(api.Post("/v1/trust", std::string("{\"workbook_id\":\"") + WORKBOOK + "\",\"hashes\":[\"" + hash +
	                                      "\",\"" + stranger + "\"]}"),
	            422, "E_BAD_REQUEST");
	CHECK(daemon.Value("SELECT count(*) FROM meta.trusted_definitions WHERE definition_hash = " + SqlString(hash)) ==
	      "0");
	CheckStatus(api.Post("/v1/trust", "{\"workbook_id\":\"not-a-uuid\",\"hashes\":[]}"), 422, "E_BAD_REQUEST");
	CheckStatus(api.Post("/v1/trust", std::string("{\"workbook_id\":\"") + WORKBOOK + "\"}"), 422, "E_BAD_REQUEST");
	CheckStatus(api.Post("/v1/trust", "[]"), 400, "E_BAD_REQUEST");

	const auto trusted = api.Post("/v1/trust", std::string("{\"workbook_id\":\"") + WORKBOOK + "\",\"hashes\":[\"" +
	                                               hash + "\",\"" + hash + "\"]}");
	CheckStatus(trusted, 200);
	CHECK(trusted->body == "{\"trusted\":[\"" + hash + "\"]}");
	CHECK(daemon.Value("SELECT origin || ' ' || workbook_id FROM meta.trusted_definitions WHERE definition_hash = " +
	                   SqlString(hash)) == std::string("user_consent ") + WORKBOOK);
	CHECK(Contains(api.Get(std::string("/v1/queries/shared?workbook=") + WORKBOOK)->body, "\"trusted\":true"));

	script.Answer(Page(InvoiceRows()));
	const auto run_id = RunAndWait(api, body);
	CHECK(Field(api.Get("/v1/runs/" + run_id)->body, "status") == "succeeded");

	// A changed text is a new hash: not trusted until confirmed again.
	const auto changed = Query("shared", "", "SELECT invoice_id, amt, note FROM ap_invoices WHERE 1 = 1");
	CheckStatus(api.Put(std::string("/v1/workbooks/") + WORKBOOK + "/queries",
	                    "{\"queries\":[{\"text\":" + JsonString(changed) + "}]}"),
	            200);
	CheckStatus(api.Post("/v1/runs", RunBody(changed, std::string(",\"workbook_id\":\"") + WORKBOOK + "\"")), 403,
	            "E_NOT_TRUSTED");

	// Saved in the editor: trusted, and so is the same text in a workbook.
	const auto edited = Query("edited", "", "SELECT invoice_id FROM ap_invoices");
	const auto saved = api.Put("/v1/queries/edited", "{\"project\":\"mine\",\"text\":" + JsonString(edited) + "}");
	CheckStatus(saved, 200);
	CHECK(Contains(saved->body, "\"trusted\":true"));
	CHECK(daemon.Value("SELECT origin FROM meta.trusted_definitions WHERE definition_hash = " +
	                   SqlString(ofexcel::DefinitionHash(edited))) == "local_editor");
	const auto copied = api.Put(std::string("/v1/workbooks/") + WORKBOOK + "/queries",
	                            "{\"queries\":[{\"text\":" + JsonString(edited) + "}]}");
	CheckStatus(copied, 200);
	CHECK(Contains(copied->body, "\"trusted\":true"));
}

// --- C4: runs and datasets ------------------------------------------------------

//! POST /v1/runs, GET /v1/runs/{id}, its rows as a RowsPage in §3.2's encoding.
void TestRunAndRows(Daemon &daemon) {
	Api api(daemon.port);
	CheckStatus(api.Put("/v1/queries/invoices", "{\"project\":\"ap\",\"text\":" + JsonString(INVOICES) + "}"), 200);

	script.Answer(Page(InvoiceRows()));
	const auto started = api.Post("/v1/runs", RunBody(INVOICES, ",\"params\":{\"bu\":204,\"period\":\"2026-02\"}"));
	CheckStatus(started, 202);
	const auto run_id = Field(started->body, "run_id");
	CHECK(run_id.size() == 36);
	CHECK(Field(started->body, "query", "id") == "invoices");
	CHECK(Field(started->body, "query", "hash") == ofexcel::DefinitionHash(INVOICES));
	CHECK(Field(started->body, "connection") == "fusion");
	CHECK(Contains(started->body, "\"params\":{\"bu\":\"204\",\"period\":\"2026-02\"}"));
	CHECK(Contains(started->body, "\"depends_on\":[]"));
	const auto started_at = Field(started->body, "started_at");
	CHECK(started_at.size() == 20 && started_at[10] == 'T' && started_at.back() == 'Z');
	ofexcel::RunEngine::Get().Wait(run_id);
	// The JSON number reached Oracle as written (§3.1).
	CHECK(Contains(script.FirstSql(), "bu = 204"));

	const auto run = api.Get("/v1/runs/" + run_id);
	CheckStatus(run, 200);
	CHECK(Field(run->body, "status") == "succeeded");
	CHECK(Field(run->body, "rows_fetched") == "2");
	CHECK(Field(run->body, "truncated") == "false");
	CHECK(Field(run->body, "finished_at").size() == 20);
	CHECK(Field(run->body, "error") == "");
	CHECK(Field(run->body, "instance_key").size() == 64);

	const auto rows = api.Get("/v1/runs/" + run_id + "/rows");
	CheckStatus(rows, 200);
	CHECK(rows->body == "{\"columns\":[{\"name\":\"INVOICE_ID\",\"header\":\"INVOICE_ID\",\"type\":\"id\"},"
	                    "{\"name\":\"AMT\",\"header\":\"AMT\",\"type\":\"decimal(38,2)\"},"
	                    "{\"name\":\"NOTE\",\"header\":\"NOTE\",\"type\":\"string\"}],"
	                    "\"rows\":[[\"101\",\"0.84\",\"first\"],[\"102\",\"-1.5\",null]],"
	                    "\"offset\":0,\"limit\":5000,\"total\":2,\"truncated\":false}");
	const auto second = api.Get("/v1/runs/" + run_id + "/rows?offset=1&limit=1");
	CheckStatus(second, 200);
	CHECK(Contains(second->body, "\"rows\":[[\"102\",\"-1.5\",null]],\"offset\":1,\"limit\":1,\"total\":2"));
	CheckStatus(api.Get("/v1/runs/" + run_id + "/rows?limit=20001"), 422, "E_BAD_REQUEST");
	CheckStatus(api.Get("/v1/runs/" + run_id + "/rows?limit=0"), 422, "E_BAD_REQUEST");
	CheckStatus(api.Get("/v1/runs/" + run_id + "/rows?offset=-1"), 422, "E_BAD_REQUEST");
	CheckStatus(api.Get("/v1/runs/" + run_id + "/rows?limit=20000"), 200);

	CheckStatus(api.Get("/v1/runs/no-such-run"), 404, "E_NOT_FOUND");
	CheckStatus(api.Get("/v1/runs/no-such-run/rows"), 404, "E_NOT_FOUND");
	CheckStatus(api.Post("/v1/runs/no-such-run/cancel", "{}"), 404, "E_NOT_FOUND");
}

//! What POST /v1/runs refuses before a run exists.
void TestRefusedStarts(Daemon &daemon) {
	Api api(daemon.port);
	const auto runs_before = daemon.Value("SELECT count(*) FROM meta.runs");
	CheckStatus(api.Post("/v1/runs", "{}"), 422, "E_BAD_REQUEST");
	CheckStatus(api.Post("/v1/runs", "not json"), 400, "E_BAD_REQUEST");
	CheckStatus(api.Post("/v1/runs", "{\"query\":{\"id\":\"invoices\",\"hash\":\"00\"}}"), 404, "E_NOT_FOUND");
	CheckStatus(api.Post("/v1/runs", RunBody(INVOICES, ",\"deps\":\"some\"")), 422, "E_BAD_REQUEST");
	CheckStatus(api.Post("/v1/runs", RunBody(INVOICES, ",\"params\":[1]")), 422, "E_BAD_REQUEST");
	CheckStatus(api.Post("/v1/runs", RunBody(INVOICES, ",\"force\":\"yes\"")), 422, "E_BAD_REQUEST");
	CheckStatus(api.Post("/v1/runs", RunBody(INVOICES, ",\"params\":{\"bu\":1.5}")), 422, "E_PARAM_TYPE");
	CheckStatus(api.Post("/v1/runs", RunBody(INVOICES, ",\"params\":{\"bu\":{}}")), 422, "E_PARAM_TYPE");
	CheckStatus(api.Post("/v1/runs", RunBody(INVOICES, ",\"connection\":\"nowhere\"")), 404, "E_NOT_FOUND");
	CHECK(daemon.Value("SELECT count(*) FROM meta.runs") == runs_before);
}

//! A run that is going: rows not yet, a second start joins it, force and
//! DELETE are refused, cancel ends it within 5 s and keeps the data.
void TestRunningAndCancel(Daemon &daemon) {
	Api api(daemon.port);
	const auto body = RunBody(INVOICES, ",\"params\":{\"period\":\"2026-03\"}");
	script.Answer(Page(InvoiceRows()));
	const auto first = RunAndWait(api, body);
	const auto instance_key = Field(api.Get("/v1/runs/" + first)->body, "instance_key");

	script.Answer(Page(InvoiceRows()), true);
	const auto started = api.Post("/v1/runs", body);
	CheckStatus(started, 202);
	const auto run_id = Field(started->body, "run_id");
	WaitUntilHolding();
	CHECK(Field(api.Get("/v1/runs/" + run_id)->body, "status") == "running");
	CHECK(Field(api.Get("/v1/status")->body, "active_runs") == "1");
	CheckStatus(api.Get("/v1/runs/" + run_id + "/rows"), 409, "E_RUN_NOT_FINISHED");

	const auto joined = api.Post("/v1/runs", body);
	CheckStatus(joined, 202);
	CHECK(Field(joined->body, "run_id") == run_id);
	CheckStatus(api.Post("/v1/runs", RunBody(INVOICES, ",\"params\":{\"period\":\"2026-03\"},\"force\":true")), 409,
	            "E_CONFLICT");
	CheckStatus(api.Delete("/v1/datasets/" + instance_key), 409, "E_CONFLICT");

	const auto cancelled_at = std::chrono::steady_clock::now();
	const auto cancelled = api.Post("/v1/runs/" + run_id + "/cancel", "{}");
	CheckStatus(cancelled, 200);
	CHECK(std::chrono::steady_clock::now() - cancelled_at < std::chrono::seconds(5));
	CHECK(Field(cancelled->body, "status") == "cancelled");
	CHECK(Field(cancelled->body, "error", "code") == "E_CANCELLED");
	// Again: a finished run is left as it is.
	const auto again = api.Post("/v1/runs/" + run_id + "/cancel", "{}");
	CheckStatus(again, 200);
	CHECK(again->body == cancelled->body);
	CheckStatus(api.Get("/v1/runs/" + run_id + "/rows"), 409, "E_RUN_NOT_FINISHED");
	CHECK(Field(api.Get("/v1/status")->body, "active_runs") == "0");
	// The data from before is still there.
	CheckStatus(api.Get("/v1/datasets/" + instance_key + "/rows"), 200);
	CheckStatus(api.Get("/v1/runs/" + first + "/rows"), 200);
}

//! The rows of a version two runs ago are gone: 410 E_GONE.
void TestGone(Daemon &daemon) {
	Api api(daemon.port);
	const auto body = RunBody(INVOICES, ",\"params\":{\"period\":\"2026-04\"}");
	script.Answer(Page(InvoiceRows()));
	const auto first = RunAndWait(api, body);
	const auto second = RunAndWait(api, body);
	CheckStatus(api.Get("/v1/runs/" + first + "/rows"), 200);
	const auto third = RunAndWait(api, body);
	CheckStatus(api.Get("/v1/runs/" + first + "/rows"), 410, "E_GONE");
	CheckStatus(api.Get("/v1/runs/" + second + "/rows"), 200);
	CheckStatus(api.Get("/v1/runs/" + third + "/rows"), 200);
	(void)daemon;
}

//! GET /v1/runs: newest first, filtered by query and workbook.
void TestListRuns(Daemon &daemon) {
	Api api(daemon.port);
	const auto listed = api.Get("/v1/runs?query=invoices&limit=2");
	CheckStatus(listed, 200);
	const auto document = ofexcel::JsonDocument::Parse(listed->body);
	const auto runs = document.Root().Get("runs");
	CHECK(runs.Size() == 2);
	const auto newest = daemon.Value("SELECT run_id FROM meta.runs WHERE query_id = 'invoices' "
	                                 "ORDER BY started_at DESC, run_id LIMIT 1");
	CHECK(runs.At(0).Get("run_id").String() == newest);
	CHECK(Contains(api.Get("/v1/runs?query=INVOICES&limit=1")->body, newest));
	const auto workbook_runs = api.Get(std::string("/v1/runs?workbook=") + WORKBOOK);
	CheckStatus(workbook_runs, 200);
	// The one run TestTrust started for the workbook's query.
	const auto workbook_document = ofexcel::JsonDocument::Parse(workbook_runs->body);
	CHECK(workbook_document.Root().Get("runs").Size() == 1);
	CHECK(workbook_document.Root().Get("runs").At(0).Get("query").Get("id").String() == "shared");
	CHECK(ofexcel::JsonDocument::Parse(api.Get("/v1/runs?query=nothing")->body).Root().Get("runs").Size() == 0);
	CheckStatus(api.Get("/v1/runs?limit=0"), 422, "E_BAD_REQUEST");
	CheckStatus(api.Get("/v1/runs?limit=1001"), 422, "E_BAD_REQUEST");
}

//! GET /v1/datasets, a dataset's rows, an empty one, DELETE.
void TestDatasets(Daemon &daemon) {
	Api api(daemon.port);
	const auto body = RunBody(INVOICES, ",\"params\":{\"period\":\"2026-05\"}");
	script.Answer(Page(InvoiceRows()));
	const auto run_id = RunAndWait(api, body);
	const auto instance_key = Field(api.Get("/v1/runs/" + run_id)->body, "instance_key");

	const auto listed = api.Get("/v1/datasets");
	CheckStatus(listed, 200);
	CHECK(Contains(listed->body, "{\"instance_key\":\"" + instance_key +
	                                 "\",\"query\":{\"id\":\"invoices\",\"hash\":\"" +
	                                 ofexcel::DefinitionHash(INVOICES) +
	                                 "\"},\"connection\":\"fusion\",\"params\":{\"bu\":\"1\",\"period\":\"2026-05\"},"
	                                 "\"status\":\"ready\",\"fetched_at\":\""));
	CHECK(Contains(listed->body, "\"row_count\":2,\"truncated\":false}"));

	const auto rows = api.Get("/v1/datasets/" + instance_key + "/rows?limit=1");
	CheckStatus(rows, 200);
	CHECK(Contains(rows->body, "\"rows\":[[\"101\",\"0.84\",\"first\"]],\"offset\":0,\"limit\":1,\"total\":2"));
	CheckStatus(api.Get("/v1/datasets/no-such-key/rows"), 404, "E_NOT_FOUND");

	// An instance whose only run failed has no data yet.
	const auto unknown = Query("unknown", "", "SELECT x FROM t WHERE 1 = 0");
	CheckStatus(api.Put("/v1/queries/unknown", "{\"project\":\"ap\",\"text\":" + JsonString(unknown) + "}"), 200);
	script.Answer(EmptyPage());
	const auto failed = RunAndWait(api, RunBody(unknown));
	const auto failed_run = api.Get("/v1/runs/" + failed);
	CHECK(Field(failed_run->body, "status") == "failed");
	CHECK(Field(failed_run->body, "error", "code") == "E_EMPTY_UNKNOWN_SCHEMA");
	CheckStatus(api.Get("/v1/runs/" + failed + "/rows"), 409, "E_RUN_NOT_FINISHED");
	const auto empty_key = Field(failed_run->body, "instance_key");
	CheckStatus(api.Get("/v1/datasets/" + empty_key + "/rows"), 409, "E_DEPENDENCY_MISSING");
	CHECK(Contains(api.Get("/v1/datasets")->body,
	               "\"status\":\"empty\",\"last_error\":{\"code\":\"E_EMPTY_UNKNOWN_SCHEMA\""));

	const auto table = daemon.Value("SELECT current_table FROM meta.dataset_instances WHERE instance_key = " +
	                                SqlString(instance_key));
	CHECK(daemon.TableExists(table));
	const auto before = std::stoll(api.Get("/v1/health")->get_header_value("X-OF-Data-Version"));
	const auto deleted = api.Delete("/v1/datasets/" + instance_key);
	CheckStatus(deleted, 204);
	CHECK(std::stoll(deleted->get_header_value("X-OF-Data-Version")) == before + 1);
	CHECK(!daemon.TableExists(table));
	CheckStatus(api.Get("/v1/datasets/" + instance_key + "/rows"), 404, "E_NOT_FOUND");
	CheckStatus(api.Delete("/v1/datasets/" + instance_key), 404, "E_NOT_FOUND");
	// The run stays in the history; its rows are gone.
	CheckStatus(api.Get("/v1/runs/" + run_id), 200);
	CheckStatus(api.Get("/v1/runs/" + run_id + "/rows"), 410, "E_GONE");
}

} // namespace

int main() {
	test_support::TemporaryDirectory directory("ofexcel_runs_api_test");
	const auto home = ofexcel::JoinPath(directory.path, "home");
	SetQexcelHome(home);
	const auto tls = ofexcel::TlsFilesIn(ofexcel::DefaultTlsDir());
	ofexcel::InstallLocalCa(tls.dir);
	ca_cert = tls.ca_cert;
	// Never the user's own ~/QExcel/projects.
	WriteFile(ofexcel::JoinPath(home, ofexcel::INIT_FILE_NAME),
	          "SET ofexcel_projects_dir = " + SqlString(ofexcel::JoinPath(directory.path, "projects")) +
	              ";\nCREATE SECRET fusion (TYPE oracle_fusion, "
	              "ENDPOINT 'https://fusion.example.com/xmlpserver/services/ExternalReportWSSService?WSDL', "
	              "REPORT_PATH '/Custom/Financials/RP_ARB.xdo', USERNAME 'analyst', PASSWORD 'hunter2');\n");
	auto installed = ofquack::ScopedTransportFactory(
	    [](const ofquack::FusionConfig &) { return std::make_shared<FakeTransport>(); });

	{
		Daemon daemon(ofexcel::JoinPath(directory.path, "data.duckdb"));
		TestTrust(daemon);
		TestRunAndRows(daemon);
		TestRefusedStarts(daemon);
		TestRunningAndCancel(daemon);
		TestGone(daemon);
		TestListRuns(daemon);
		TestDatasets(daemon);
	}
	std::cout << "runs_api_test: all checks passed\n";
	return 0;
}
