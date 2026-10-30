// Oracle runs (TASKS.md C3) through ofquack's transport seam: the result's
// types and stored values, an empty result, the row limit, Oracle's refusal,
// cancellation and the timeout, versions of an instance, one run per instance,
// and the cleanup at start. The database is prepared as `ofexceld serve`
// prepares it; no server, no Fusion, no network.

#include "base64.h"
#include "daemon/startup.hpp"
#include "ofexcel/api_error.hpp"
#include "ofexcel/hashing.hpp"
#include "ofexcel/query_registry.hpp"
#include "ofexcel/run_engine.hpp"
#include "ofquack/errors.hpp"
#include "ofquack/transport.hpp"
#include "temporary_directory.hpp"
#include "test_support.hpp"

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
using test_support::Run;
using test_support::RunOk;

namespace fs = std::filesystem;

namespace {

// --- The fake BI Publisher ------------------------------------------------------

std::string MakeSoapResponse(const std::string &report_xml) {
	const auto encoded = base64_encode(reinterpret_cast<const unsigned char *>(report_xml.c_str()), report_xml.size());
	return "<soap:Envelope xmlns:soap=\"http://www.w3.org/2003/05/soap-envelope\"><soap:Body>"
	       "<ns2:runReportResponse><ns2:runReportReturn><ns2:reportBytes>" +
	       encoded + "</ns2:reportBytes></ns2:runReportReturn></ns2:runReportResponse></soap:Body></soap:Envelope>";
}

using Row = std::vector<std::pair<std::string, std::string>>;

//! A page as dbms_xmlgen writes it, escaped inside RESULT. A NULL value has no
//! element at all, as in the real report.
std::string Page(const std::vector<Row> &rows) {
	std::string rowset = "&lt;ROWSET&gt;";
	for (const auto &row : rows) {
		rowset += "&lt;ROW&gt;";
		for (const auto &cell : row) {
			rowset += "&lt;" + cell.first + "&gt;" + cell.second + "&lt;/" + cell.first + "&gt;";
		}
		rowset += "&lt;/ROW&gt;";
	}
	rowset += "&lt;/ROWSET&gt;";
	return MakeSoapResponse("<DATA_DS><G_1><RESULT>" + rowset + "</RESULT></G_1></DATA_DS>");
}

std::string EmptyPage() {
	return MakeSoapResponse("<DATA_DS><G_1><RESULT>&lt;ROWSET/&gt;</RESULT></G_1></DATA_DS>");
}

std::string Rejection(const std::string &oracle_error) {
	return "<soap:Envelope xmlns:soap=\"http://www.w3.org/2003/05/soap-envelope\"><soap:Body><soap:Fault>"
	       "<soap:Reason><soap:Text>" +
	       oracle_error + "</soap:Text></soap:Reason></soap:Fault></soap:Body></soap:Envelope>";
}

bool AsksForALaterPage(const std::string &sql) {
	const auto at = sql.find("OFFSET ");
	return at != std::string::npos && sql.compare(at + 7, 1, "0") != 0;
}

//! What the next requests get. Tests run one after another, so one script.
struct Script {
	std::mutex mutex;
	std::string first_page;
	//! Hold every request until it is cancelled, as a slow report would.
	bool hold = false;
	std::atomic<int> holding {0};
	std::vector<std::string> sql;

	void Answer(std::string page, bool hold_p = false) {
		std::lock_guard<std::mutex> lock(mutex);
		first_page = std::move(page);
		hold = hold_p;
		sql.clear();
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

// --- The daemon -------------------------------------------------------------

void WriteFile(const std::string &path, const std::string &content) {
	fs::create_directories(fs::u8path(path).parent_path());
	std::ofstream file(fs::u8path(path), std::ios::binary);
	file << content;
	CHECK(file);
}

std::string directory_path;

struct Daemon;
//! The daemon the tests talk to now; Request trusts its texts there.
Daemon *current_daemon = nullptr;

//! The database as serve prepares it, with a secret named fusion that the fake
//! transport answers for. `init` is added to init.sql.
struct Daemon {
	ofexcel::PreparedDatabase prepared;
	std::unique_ptr<duckdb::Connection> connection;

	explicit Daemon(const std::string &name, const std::string &init = "") {
		const auto init_file = directory_path + "/" + name + ".sql";
		WriteFile(init_file, "SET ofexcel_projects_dir = '" + directory_path + "/projects';\n"
		                     "CREATE SECRET fusion (TYPE oracle_fusion, "
		                     "ENDPOINT 'https://fusion.example.com/xmlpserver/services/ExternalReportWSSService?WSDL', "
		                     "REPORT_PATH '/Custom/Financials/RP_ARB.xdo', USERNAME 'analyst', PASSWORD 'hunter2');\n" +
		                         init);
		ofexcel::StartupOptions options;
		options.db_path = directory_path + "/" + name + ".duckdb";
		options.init_file = init_file;
		prepared = ofexcel::PrepareDaemonDatabase(options);
		connection = std::make_unique<duckdb::Connection>(*prepared.db);
		current_daemon = this;
	}

	std::string Value(const std::string &sql) {
		const auto result = RunOk(*connection, sql);
		CHECK(result.rows.size() == 1);
		return result.At(0, 0).IsNull() ? "NULL" : result.At(0, 0).ToString();
	}

	std::string RunValue(const std::string &run_id, const std::string &column) {
		return Value("SELECT " + column + " FROM meta.runs WHERE run_id = '" + run_id + "'");
	}

	std::string InstanceValue(const std::string &instance_key, const std::string &column) {
		return Value("SELECT " + column + " FROM meta.dataset_instances WHERE instance_key = '" + instance_key + "'");
	}

	bool TableExists(const std::string &table) {
		return Value("SELECT count(*) FROM duckdb_tables() WHERE schema_name = 'ds' AND table_name = '" + table +
		             "'") == "1";
	}
};

std::string Query(const std::string &id, const std::string &extra, const std::string &body) {
	return "-- @id: " + id + "\n-- @engine: oracle\n-- @version: 1\n-- @connection: fusion\n" + extra + body + "\n";
}

//! A request for `text` as it is, trusted or not.
ofexcel::RunRequest Untrusted(const std::string &text, std::map<std::string, ofexcel::ParamInput> params = {}) {
	ofexcel::RunRequest request;
	const auto parsed = ofexcel::ParseQueryFile(text);
	CHECK(parsed.Ok());
	request.definition.query_id = parsed.header.id;
	request.definition.definition_hash = ofexcel::DefinitionHash(text);
	request.definition.text = text;
	request.params = std::move(params);
	return request;
}

//! A request for `text`, trusted as the editor would trust it (SPEC.md §8.3):
//! these texts are registered nowhere.
ofexcel::RunRequest Request(const std::string &text, std::map<std::string, ofexcel::ParamInput> params = {}) {
	CHECK(current_daemon);
	RunOk(*current_daemon->connection, "INSERT INTO meta.trusted_definitions VALUES ('" + ofexcel::DefinitionHash(text) +
	                                       "', 'test', 'local_editor', NULL, TIMESTAMP '2026-01-01 00:00:00') "
	                                       "ON CONFLICT DO NOTHING");
	return Untrusted(text, std::move(params));
}

std::string StartAndWait(Daemon &daemon, const ofexcel::RunRequest &request) {
	auto &engine = ofexcel::RunEngine::Get();
	const auto started = engine.Start(*daemon.prepared.db->instance, request);
	CHECK(!started.joined);
	engine.Wait(started.run_id);
	return started.run_id;
}

void WaitUntilHolding() {
	const auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds(30);
	while (script.holding.load() == 0) {
		CHECK(std::chrono::steady_clock::now() < give_up);
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
}

const std::string INVOICES =
    Query("invoices", "-- @param period: string = 2026-01\n",
          "SELECT invoice_id, amt, invoice_date, created, updated_tz, note FROM ap_invoices WHERE period = :period");

//! A date parameter refuses a number (§3.1).
const std::string DATED = Query("dated", "-- @param d: date = 2026-01-01\n", "SELECT n FROM t WHERE d = :d");

std::vector<Row> InvoiceRows() {
	return {{{"INVOICE_ID", "101"},
	         {"AMT", ".84"},
	         {"INVOICE_DATE", "2026-01-01"},
	         {"CREATED", "2026-01-01 10:20:30.123456789"},
	         {"UPDATED_TZ", "2026-01-01 01:00:00.5 +03:00"},
	         {"NOTE", "first"}},
	        {{"INVOICE_ID", "102"},
	         {"AMT", "-1.5"},
	         {"INVOICE_DATE", "2026-02-01"},
	         {"CREATED", "2026-02-01 00:00:00.000000"},
	         {"UPDATED_TZ", "2026-01-31 23:30:00 -01:00"}}};
}

// --- Tests ------------------------------------------------------------------

//! Types decided over the whole result (§3.3), values stored in their contract
//! form, '' as NULL, the swap of §6.4 and the schema kept for the next run.
void TestSuccessfulRun(Daemon &daemon) {
	script.Answer(Page(InvoiceRows()));
	const auto data_version = std::stoll(daemon.Value("SELECT value FROM meta.state WHERE key = 'data_version'"));
	const auto run_id = StartAndWait(daemon, Request(INVOICES));

	CHECK(daemon.RunValue(run_id, "status") == "succeeded");
	CHECK(daemon.RunValue(run_id, "rows_fetched") == "2");
	CHECK(daemon.RunValue(run_id, "truncated") == "false");
	CHECK(daemon.RunValue(run_id, "warnings_json") == "[]");
	CHECK(daemon.RunValue(run_id, "error_code") == "NULL");
	CHECK(daemon.RunValue(run_id, "user_name") == "NULL");
	// The rendered SQL went to ofquack, with the default substituted (§4).
	CHECK(!script.sql.empty());
	CHECK(Contains(script.sql.front(), "period = '2026-01'"));

	const auto key = daemon.RunValue(run_id, "instance_key");
	CHECK(daemon.InstanceValue(key, "status") == "ready");
	CHECK(daemon.InstanceValue(key, "current_version") == "1");
	CHECK(daemon.InstanceValue(key, "row_count") == "2");
	CHECK(daemon.InstanceValue(key, "prev_table") == "NULL");
	CHECK(daemon.InstanceValue(key, "params_json") == "{\"period\":\"2026-01\"}");
	const auto table = daemon.InstanceValue(key, "current_table");
	CHECK(table == "invoices__" + key.substr(0, 12) + "__v1");
	CHECK(daemon.RunValue(run_id, "result_table") == table);
	CHECK(std::stoll(daemon.Value("SELECT value FROM meta.state WHERE key = 'data_version'")) == data_version + 1);
	CHECK(daemon.Value("SELECT count(*) FROM duckdb_tables() WHERE schema_name = 'ds' AND "
	                   "starts_with(table_name, 'stage__')") == "0");

	const auto types = RunOk(*daemon.connection, "SELECT column_name, data_type FROM duckdb_columns() "
	                                             "WHERE schema_name = 'ds' AND table_name = '" +
	                                                 table + "' ORDER BY column_index");
	const std::vector<std::pair<std::string, std::string>> expected_types = {
	    {"INVOICE_ID", "VARCHAR"},     {"AMT", "DECIMAL(38,2)"},  {"INVOICE_DATE", "DATE"},
	    {"CREATED", "TIMESTAMP"},      {"UPDATED_TZ", "TIMESTAMP"}, {"NOTE", "VARCHAR"}};
	CHECK(types.rows.size() == expected_types.size());
	for (size_t i = 0; i < expected_types.size(); i++) {
		CHECK(types.At(i, 0).ToString() == expected_types[i].first);
		CHECK(types.At(i, 1).ToString() == expected_types[i].second);
	}

	const auto schema = RunOk(*daemon.connection, "SELECT column_name, contract_type FROM meta.query_schemas "
	                                              "WHERE definition_hash = '" +
	                                                  ofexcel::DefinitionHash(INVOICES) + "' ORDER BY ordinal");
	const std::vector<std::string> contract_types = {"id", "decimal(38,2)", "date", "timestamp", "timestamptz",
	                                                 "string"};
	CHECK(schema.rows.size() == contract_types.size());
	for (size_t i = 0; i < contract_types.size(); i++) {
		CHECK(schema.At(i, 1).ToString() == contract_types[i]);
	}

	const auto rows = RunOk(*daemon.connection, "SELECT * FROM ds.\"" + table + "\"");
	CHECK(rows.rows.size() == 2);
	CHECK(rows.At(0, 0).ToString() == "101");
	CHECK(rows.At(0, 1).ToString() == "0.84");
	CHECK(rows.At(1, 1).ToString() == "-1.50");
	CHECK(rows.At(0, 2).ToString() == "2026-01-01");
	// Cut to microseconds, not rounded (§3.3).
	CHECK(rows.At(0, 3).ToString() == "2026-01-01 10:20:30.123456");
	CHECK(rows.At(1, 3).ToString() == "2026-02-01 00:00:00");
	// Moved to UTC across midnight.
	CHECK(rows.At(0, 4).ToString() == "2025-12-31 22:00:00.5");
	CHECK(rows.At(1, 4).ToString() == "2026-02-01 00:30:00");
	CHECK(rows.At(0, 5).ToString() == "first");
	CHECK(rows.At(1, 5).IsNull());
}

//! §5.2: an empty result with a known schema is a dataset of 0 rows; without
//! one it fails and the instance stays empty.
void TestEmptyResult(Daemon &daemon) {
	script.Answer(EmptyPage());

	// The schema the successful run stored, for other parameters.
	const auto known = StartAndWait(daemon, Request(INVOICES, {{"period", ofexcel::ParamInput::String("2026-09")}}));
	CHECK(daemon.RunValue(known, "status") == "succeeded");
	CHECK(daemon.RunValue(known, "rows_fetched") == "0");
	const auto known_table = daemon.RunValue(known, "result_table");
	CHECK(daemon.Value("SELECT count(*) FROM ds.\"" + known_table + "\"") == "0");
	CHECK(daemon.Value("SELECT data_type FROM duckdb_columns() WHERE table_name = '" + known_table +
	                   "' AND column_name = 'AMT'") == "DECIMAL(38,2)");

	const auto declared_text =
	    Query("declared", "-- @column VENDOR_ID: id | Vendor\n-- @column AMOUNT: decimal(18,2)\n",
	          "SELECT vendor_id, amount FROM ap_invoices WHERE 1 = 0");
	const auto declared = StartAndWait(daemon, Request(declared_text));
	CHECK(daemon.RunValue(declared, "status") == "succeeded");
	const auto declared_table = daemon.RunValue(declared, "result_table");
	CHECK(daemon.Value("SELECT count(*) FROM ds.\"" + declared_table + "\"") == "0");
	CHECK(daemon.Value("SELECT string_agg(column_name || ' ' || data_type, ', ' ORDER BY column_index) "
	                   "FROM duckdb_columns() WHERE table_name = '" +
	                   declared_table + "'") == "VENDOR_ID VARCHAR, AMOUNT DECIMAL(18,2)");

	const auto unknown = StartAndWait(daemon, Request(Query("unknown", "", "SELECT 1 x FROM dual WHERE 1 = 0")));
	CHECK(daemon.RunValue(unknown, "status") == "failed");
	CHECK(daemon.RunValue(unknown, "error_code") == "E_EMPTY_UNKNOWN_SCHEMA");
	CHECK(Contains(daemon.RunValue(unknown, "error_message"), "@column"));
	CHECK(daemon.RunValue(unknown, "result_table") == "NULL");
	const auto key = daemon.RunValue(unknown, "instance_key");
	CHECK(daemon.InstanceValue(key, "status") == "empty");
	CHECK(daemon.InstanceValue(key, "current_table") == "NULL");
	CHECK(daemon.InstanceValue(key, "last_error_code") == "E_EMPTY_UNKNOWN_SCHEMA");
}

//! §5.4: row 11 of a query limited to 10 marks the run truncated.
void TestRowLimit(Daemon &daemon) {
	std::vector<Row> rows;
	for (int i = 1; i <= 11; i++) {
		rows.push_back({{"N", std::to_string(i)}});
	}
	script.Answer(Page(rows));
	const auto limited = StartAndWait(daemon, Request(Query("limited", "-- @max_rows: 10\n", "SELECT n FROM t")));
	CHECK(daemon.RunValue(limited, "status") == "succeeded");
	CHECK(daemon.RunValue(limited, "rows_fetched") == "10");
	CHECK(daemon.RunValue(limited, "truncated") == "true");
	const auto table = daemon.RunValue(limited, "result_table");
	CHECK(daemon.Value("SELECT count(*) FROM ds.\"" + table + "\"") == "10");
	CHECK(daemon.Value("SELECT max(N) FROM ds.\"" + table + "\"") == "10");
	CHECK(daemon.InstanceValue(daemon.RunValue(limited, "instance_key"), "truncated") == "true");

	// Exactly the limit is not truncated.
	rows.pop_back();
	script.Answer(Page(rows));
	const auto exact = StartAndWait(daemon, Request(Query("exact", "-- @max_rows: 10\n", "SELECT n FROM t")));
	CHECK(daemon.RunValue(exact, "rows_fetched") == "10");
	CHECK(daemon.RunValue(exact, "truncated") == "false");
}

//! §6.4: each run makes a new version; the one before becomes previous and the
//! one before that is dropped.
void TestVersions(Daemon &daemon) {
	const auto request = Request(INVOICES, {{"period", ofexcel::ParamInput::String("2026-02")}});
	script.Answer(Page(InvoiceRows()));
	const auto first = StartAndWait(daemon, request);
	const auto key = daemon.RunValue(first, "instance_key");
	const auto v1 = daemon.InstanceValue(key, "current_table");
	CHECK(v1 == "invoices__" + key.substr(0, 12) + "__v1");

	const auto second = StartAndWait(daemon, request);
	CHECK(second != first);
	const auto v2 = daemon.InstanceValue(key, "current_table");
	CHECK(v2 == "invoices__" + key.substr(0, 12) + "__v2");
	CHECK(daemon.InstanceValue(key, "prev_table") == v1);
	CHECK(daemon.InstanceValue(key, "data_version") == "2");
	CHECK(daemon.TableExists(v1));

	StartAndWait(daemon, request);
	CHECK(daemon.InstanceValue(key, "current_table") == "invoices__" + key.substr(0, 12) + "__v3");
	CHECK(daemon.InstanceValue(key, "prev_table") == v2);
	CHECK(!daemon.TableExists(v1));
	CHECK(daemon.TableExists(v2));
}

//! §14.1: Oracle's refusal is E_FUSION_REJECTED, and the instance keeps the
//! version it had.
void TestOracleRejects(Daemon &daemon) {
	const auto request = Request(INVOICES, {{"period", ofexcel::ParamInput::String("2026-03")}});
	script.Answer(Page(InvoiceRows()));
	const auto good = StartAndWait(daemon, request);
	const auto key = daemon.RunValue(good, "instance_key");
	const auto table = daemon.InstanceValue(key, "current_table");

	script.Answer(Rejection("ORA-00942: table or view does not exist"));
	const auto bad = StartAndWait(daemon, request);
	CHECK(daemon.RunValue(bad, "status") == "failed");
	CHECK(daemon.RunValue(bad, "error_code") == "E_FUSION_REJECTED");
	CHECK(Contains(daemon.RunValue(bad, "error_message"), "ORA-00942"));
	CHECK(daemon.RunValue(bad, "result_table") == "NULL");
	CHECK(daemon.InstanceValue(key, "current_table") == table);
	CHECK(daemon.InstanceValue(key, "current_version") == "1");
	CHECK(daemon.InstanceValue(key, "status") == "ready");
	CHECK(daemon.InstanceValue(key, "last_error_code") == "E_FUSION_REJECTED");
	CHECK(daemon.TableExists(table));
}

//! §3.3 item 1: a value the @column type does not hold fails the run.
void TestDeclaredMismatch(Daemon &daemon) {
	script.Answer(Page({{{"AMT", "1"}}, {{"AMT", ".84"}}}));
	const auto run_id = StartAndWait(daemon, Request(Query("whole", "-- @column AMT: integer\n", "SELECT amt FROM t")));
	CHECK(daemon.RunValue(run_id, "status") == "failed");
	CHECK(daemon.RunValue(run_id, "error_code") == "E_TYPE_DECLARED_MISMATCH");
	const auto message = daemon.RunValue(run_id, "error_message");
	CHECK(Contains(message, "AMT") && Contains(message, "in row 2") && Contains(message, ".84"));
	CHECK(daemon.Value("SELECT count(*) FROM duckdb_tables() WHERE schema_name = 'ds' AND "
	                   "starts_with(table_name, 'whole__')") == "0");
}

//! Cancelling a run held inside a page: cancelled within 5 s (§8.3), the
//! previous version in place. A second start of the same instance meanwhile
//! joins the run; with force it is refused.
void TestCancelAndJoin(Daemon &daemon) {
	const auto request = Request(INVOICES, {{"period", ofexcel::ParamInput::String("2026-04")}});
	script.Answer(Page(InvoiceRows()));
	const auto good = StartAndWait(daemon, request);
	const auto key = daemon.RunValue(good, "instance_key");
	const auto table = daemon.InstanceValue(key, "current_table");

	auto &engine = ofexcel::RunEngine::Get();
	auto &db = *daemon.prepared.db->instance;
	script.Answer(Page(InvoiceRows()), true);
	const auto held = engine.Start(db, request);
	CHECK(!held.joined);
	WaitUntilHolding();
	CHECK(engine.ActiveRuns() == 1);
	CHECK(daemon.RunValue(held.run_id, "status") == "running");

	const auto again = engine.Start(db, request);
	CHECK(again.joined);
	CHECK(again.run_id == held.run_id);
	auto forced = request;
	forced.force = true;
	try {
		engine.Start(db, forced);
		CHECK(false);
	} catch (const ofexcel::ApiError &error) {
		CHECK(error.code == "E_CONFLICT");
		CHECK(error.status == 409);
	}

	const auto cancelled_at = std::chrono::steady_clock::now();
	CHECK(engine.Cancel(held.run_id));
	engine.Wait(held.run_id);
	CHECK(std::chrono::steady_clock::now() - cancelled_at < std::chrono::seconds(5));
	CHECK(!engine.Cancel(held.run_id));
	CHECK(engine.ActiveRuns() == 0);
	CHECK(daemon.RunValue(held.run_id, "status") == "cancelled");
	CHECK(daemon.RunValue(held.run_id, "error_code") == "E_CANCELLED");
	CHECK(daemon.RunValue(held.run_id, "finished_at") != "NULL");
	CHECK(daemon.InstanceValue(key, "current_table") == table);
	CHECK(daemon.InstanceValue(key, "last_error_code") == "E_CANCELLED");
	CHECK(daemon.TableExists(table));
	CHECK(daemon.Value("SELECT count(*) FROM duckdb_tables() WHERE schema_name = 'ds' AND "
	                   "starts_with(table_name, 'stage__')") == "0");
}

//! What Start refuses before a run exists.
void TestRefusedRequests(Daemon &daemon) {
	auto &db = *daemon.prepared.db->instance;
	const auto refused = [&](const ofexcel::RunRequest &request, const std::string &code) {
		try {
			ofexcel::RunEngine::Get().Start(db, request);
			CHECK(false);
		} catch (const ofexcel::ApiError &error) {
			if (error.code != code) {
				std::cerr << "expected " << code << ", got " << error.code << ": " << error.what() << "\n";
			}
			CHECK(error.code == code);
		}
	};
	const auto runs = daemon.Value("SELECT count(*) FROM meta.runs");
	// A text no project, example or confirmation vouches for (SPEC.md §8.3).
	refused(Untrusted(Query("stranger", "", "SELECT 1 FROM dual")), "E_NOT_TRUSTED");
	refused(Request(DATED, {{"d", ofexcel::ParamInput::Number("12")}}), "E_PARAM_TYPE");
	auto elsewhere = Request(INVOICES);
	elsewhere.connection = "nowhere";
	refused(elsewhere, "E_NOT_FOUND");
	refused(Request("-- @id: unbound\n-- @engine: oracle\n-- @version: 1\nSELECT 1 FROM dual\n"), "E_BAD_REQUEST");
	// Local SQL that reads what it does not declare (§7 V3).
	refused(Request("-- @id: here\n-- @engine: local\n-- @version: 1\nSELECT * FROM gl_balances\n"), "E_VALIDATION");
	CHECK(daemon.Value("SELECT count(*) FROM meta.runs") == runs);
}

//! ofexcel_run resolves the name in the registry and waits; ofexcel_runs and
//! ofexcel_datasets show what it left.
void TestSqlFunctions(Daemon &daemon) {
	ofexcel::RegisterExamples(*daemon.connection, {{"invoices.sql", INVOICES}, {"dated.sql", DATED}});
	script.Answer(Page(InvoiceRows()));
	const auto run =
	    RunOk(*daemon.connection, "SELECT * FROM ofexcel_run('invoices', params := {'period': '2026-05'})");
	CHECK(run.rows.size() == 1);
	CHECK(run.names[1] == "status");
	CHECK(run.At(0, 1).ToString() == "succeeded");
	CHECK(run.At(0, 2).GetValue<int64_t>() == 2);
	CHECK(Contains(script.sql.front(), "period = '2026-05'"));

	const auto listed = RunOk(*daemon.connection, "SELECT run_id, status FROM ofexcel_runs() LIMIT 1");
	CHECK(listed.At(0, 0).ToString() == run.At(0, 0).ToString());
	CHECK(daemon.Value("SELECT count(*) FROM ofexcel_runs()") == daemon.Value("SELECT count(*) FROM meta.runs"));
	CHECK(daemon.Value("SELECT count(*) FROM ofexcel_datasets() WHERE params_json = '{\"period\":\"2026-05\"}'") ==
	      "1");

	const auto missing = Run(*daemon.connection, "SELECT * FROM ofexcel_run('nothing')");
	CHECK(missing.HasError() && Contains(missing.error, "E_NOT_FOUND"));
	const auto bad = Run(*daemon.connection, "SELECT * FROM ofexcel_run('dated', params := {'d': 12})");
	CHECK(bad.HasError() && Contains(bad.error, "E_PARAM_TYPE"));
}

//! ofexcel_run_timeout interrupts a run that takes too long: E_TIMEOUT.
void TestTimeout() {
	Daemon daemon("timeout", "SET ofexcel_run_timeout = 1;\n");
	script.Answer(Page(InvoiceRows()), true);
	const auto started_at = std::chrono::steady_clock::now();
	const auto run_id = StartAndWait(daemon, Request(INVOICES));
	CHECK(std::chrono::steady_clock::now() - started_at < std::chrono::seconds(6));
	CHECK(daemon.RunValue(run_id, "status") == "failed");
	CHECK(daemon.RunValue(run_id, "error_code") == "E_TIMEOUT");
	CHECK(Contains(daemon.RunValue(run_id, "error_message"), "ofexcel_run_timeout"));
}

//! At start: instances not read for ofexcel_dataset_ttl_days go with their
//! tables, stage tables go, runs still marked running are failed.
void TestCleanupAtStart() {
	std::string kept;
	std::string expired;
	{
		Daemon daemon("cleanup");
		script.Answer(Page(InvoiceRows()));
		const auto first = StartAndWait(daemon, Request(INVOICES, {{"period", ofexcel::ParamInput::String("old")}}));
		const auto second = StartAndWait(daemon, Request(INVOICES, {{"period", ofexcel::ParamInput::String("new")}}));
		expired = daemon.RunValue(first, "instance_key");
		kept = daemon.RunValue(second, "instance_key");
		RunOk(*daemon.connection, "UPDATE meta.dataset_instances SET last_access_at = TIMESTAMP '2020-01-01 00:00:00' "
		                          "WHERE instance_key = '" +
		                              expired + "'");
		RunOk(*daemon.connection, "CREATE TABLE ds.stage__left (c0 VARCHAR)");
		RunOk(*daemon.connection, "INSERT INTO meta.runs (run_id, instance_key, query_id, definition_hash, "
		                          "connection_name, params_json, status, started_at) VALUES ('left-running', 'k', "
		                          "'q', 'h', 'fusion', '{}', 'running', TIMESTAMP '2026-01-01 00:00:00')");
	}
	Daemon daemon("cleanup", "SET ofexcel_dataset_ttl_days = 30;\n");
	CHECK(daemon.Value("SELECT count(*) FROM meta.dataset_instances WHERE instance_key = '" + expired + "'") == "0");
	CHECK(daemon.Value("SELECT count(*) FROM duckdb_tables() WHERE schema_name = 'ds' AND table_name LIKE '%" +
	                   expired.substr(0, 12) + "%'") == "0");
	CHECK(daemon.InstanceValue(kept, "status") == "ready");
	CHECK(daemon.TableExists(daemon.InstanceValue(kept, "current_table")));
	CHECK(!daemon.TableExists("stage__left"));
	CHECK(daemon.RunValue("left-running", "status") == "failed");
	CHECK(daemon.RunValue("left-running", "error_code") == "E_INTERNAL");
}

} // namespace

int main() {
	test_support::TemporaryDirectory directory("ofexcel_runs_test");
	fs::create_directories(fs::u8path(directory.path));
	directory_path = directory.path;
	auto installed = ofquack::ScopedTransportFactory(
	    [](const ofquack::FusionConfig &) { return std::make_shared<FakeTransport>(); });

	{
		Daemon daemon("runs");
		TestSuccessfulRun(daemon);
		TestEmptyResult(daemon);
		TestRowLimit(daemon);
		TestVersions(daemon);
		TestOracleRejects(daemon);
		TestDeclaredMismatch(daemon);
		TestCancelAndJoin(daemon);
		TestRefusedRequests(daemon);
		TestSqlFunctions(daemon);
	}
	TestTimeout();
	TestCleanupAtStart();
	std::cout << "runs_test: all checks passed\n";
	return 0;
}
