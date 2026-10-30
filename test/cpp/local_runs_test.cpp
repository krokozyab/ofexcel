// Local runs and their dependencies (TASKS.md C5): a reconciliation of an
// Oracle dataset, fetched through ofquack's transport seam, with an imported
// sheet table; the deps modes, meta.missing, a failing dependency, validation
// before a run, the types and values of a Local result, and cancellation of a
// chain. The database is prepared as `ofexceld serve` prepares it; no server,
// no Fusion, no network.

#include "base64.h"
#include "daemon/startup.hpp"
#include "ofexcel/api_error.hpp"
#include "ofexcel/hashing.hpp"
#include "ofexcel/json_body.hpp"
#include "ofexcel/query_registry.hpp"
#include "ofexcel/run_engine.hpp"
#include "ofquack/errors.hpp"
#include "ofquack/transport.hpp"
#include "temporary_directory.hpp"
#include "test_support.hpp"

#include <atomic>
#include <chrono>
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

const char *const WORKBOOK = "0b6f3c1e-2a4d-4c8e-9f10-7d2e5a6b8c9d";

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

std::string Rejection(const std::string &oracle_error) {
	return "<soap:Envelope xmlns:soap=\"http://www.w3.org/2003/05/soap-envelope\"><soap:Body><soap:Fault>"
	       "<soap:Reason><soap:Text>" +
	       oracle_error + "</soap:Text></soap:Reason></soap:Fault></soap:Body></soap:Envelope>";
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
	size_t Requests() {
		std::lock_guard<std::mutex> lock(mutex);
		return sql.size();
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

std::string Oracle(const std::string &id, const std::string &extra, const std::string &body) {
	return "-- @id: " + id + "\n-- @engine: oracle\n-- @version: 1\n-- @connection: fusion\n" + extra + body + "\n";
}

std::string Local(const std::string &id, const std::string &extra, const std::string &body) {
	return "-- @id: " + id + "\n-- @engine: local\n-- @version: 1\n" + extra + body + "\n";
}

const std::string INVOICES =
    Oracle("ap_invoices", "-- @param bu: integer\n",
           "SELECT invoice_num, amount FROM ap_invoices_all WHERE org_id = :bu ORDER BY invoice_num");

//! SPEC.md §4.2's example: invoices against the bank statement imported from
//! the sheet.
const std::string RECONCILIATION =
    Local("payment_reconciliation",
          "-- @param bu: integer\n-- @param tolerance: decimal(10,2) = 0.01\n"
          "-- @depends: ap_invoices(bu := $bu)\n-- @depends: xl_import.bank\n",
          "SELECT i.invoice_num AS invoice_num, i.amount AS invoice_amount, b.amount AS bank_amount,\n"
          "       i.amount - coalesce(b.amount, 0) AS diff\n"
          "FROM ap_invoices i LEFT JOIN bank b ON b.ref = i.invoice_num\n"
          "WHERE abs(i.amount - coalesce(b.amount, 0)) > $tolerance\n"
          "ORDER BY i.invoice_num");

//! Needs an import nobody loaded.
const std::string NEEDS_IMPORT = Local("needs_import", "-- @depends: xl_import.absent\n", "SELECT * FROM absent");

//! Two Local queries that need each other.
const std::string CYCLE_A = Local("cycle_a", "-- @depends: cycle_b()\n", "SELECT * FROM cycle_b");
const std::string CYCLE_B = Local("cycle_b", "-- @depends: cycle_a()\n", "SELECT * FROM cycle_a");

std::vector<Row> InvoiceRows() {
	return {{{"INVOICE_NUM", "INV-1"}, {"AMOUNT", "100.25"}},
	        {{"INVOICE_NUM", "INV-2"}, {"AMOUNT", "250.5"}},
	        {{"INVOICE_NUM", "INV-3"}, {"AMOUNT", "75.75"}}};
}

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
	}

	duckdb::DatabaseInstance &Db() {
		return *prepared.db->instance;
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

	std::string Runs(const std::string &where) {
		return Value("SELECT count(*) FROM meta.runs WHERE " + where);
	}

	//! Registered as examples, which are trusted by source (DECISIONS.md R8),
	//! so that @depends finds them.
	void Register(const std::vector<std::string> &texts) {
		std::vector<ofexcel::QueryText> examples;
		for (size_t i = 0; i < texts.size(); i++) {
			examples.push_back({"q" + std::to_string(i) + ".sql", texts[i]});
		}
		ofexcel::RegisterExamples(*connection, examples);
	}

	ofexcel::RunRequest Request(const std::string &query_id, std::map<std::string, ofexcel::ParamInput> params = {},
	                            ofexcel::DepsMode deps = ofexcel::DepsMode::MISSING) {
		ofexcel::RunRequest request;
		ofexcel::QueryScope scope;
		CHECK(ofexcel::FindQuery(*connection, query_id, scope, request.definition));
		request.params = std::move(params);
		request.deps = deps;
		request.workbook_id = WORKBOOK;
		return request;
	}

	std::string StartAndWait(const ofexcel::RunRequest &request) {
		auto &engine = ofexcel::RunEngine::Get();
		const auto started = engine.Start(Db(), request);
		CHECK(!started.joined);
		engine.Wait(started.run_id);
		return started.run_id;
	}

	ofexcel::ApiError Refused(const ofexcel::RunRequest &request) {
		try {
			ofexcel::RunEngine::Get().Start(Db(), request);
		} catch (const ofexcel::ApiError &error) {
			return error;
		}
		CHECK(false);
		return ofexcel::ApiError("", 0, "");
	}
};

ofexcel::ParamInput Integer(const std::string &text) {
	return ofexcel::ParamInput::Number(text);
}

void WaitUntilHolding() {
	const auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds(30);
	while (script.holding.load() == 0) {
		CHECK(std::chrono::steady_clock::now() < give_up);
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
}

// --- Tests ------------------------------------------------------------------

//! SPEC.md §4.2: the Oracle dependency runs first (deps = missing), as a run of
//! its own with the parent's id; the Local SQL then reads it and the import
//! under their short names, with $bu passed on and $tolerance bound.
void TestReconciliation(Daemon &daemon) {
	RunOk(*daemon.connection, "CREATE TABLE xl_import.bank (ref VARCHAR, amount DECIMAL(18,2))");
	RunOk(*daemon.connection, "INSERT INTO xl_import.bank VALUES ('INV-1', 100.25), ('INV-2', 200.00)");
	script.Answer(Page(InvoiceRows()));

	const auto run_id = daemon.StartAndWait(daemon.Request("payment_reconciliation", {{"bu", Integer("204")}}));
	if (daemon.RunValue(run_id, "status") != "succeeded") {
		std::cerr << daemon.RunValue(run_id, "error_code") << ": " << daemon.RunValue(run_id, "error_message") << "\n";
	}
	CHECK(daemon.RunValue(run_id, "status") == "succeeded");
	CHECK(daemon.RunValue(run_id, "connection_name") == "local");
	CHECK(daemon.RunValue(run_id, "environment") == "NULL");
	CHECK(daemon.RunValue(run_id, "user_name") == "NULL");
	CHECK(daemon.RunValue(run_id, "workbook_id") == WORKBOOK);
	CHECK(daemon.RunValue(run_id, "rows_fetched") == "2");
	CHECK(daemon.RunValue(run_id, "warnings_json") == "[]");
	CHECK(daemon.RunValue(run_id, "params_json") == "{\"bu\":\"204\",\"tolerance\":\"0.01\"}");

	// The dependency: one run, started for this one, with $bu substituted.
	CHECK(daemon.Runs("query_id = 'ap_invoices'") == "1");
	const auto child = daemon.Value("SELECT run_id FROM meta.runs WHERE query_id = 'ap_invoices'");
	CHECK(daemon.RunValue(child, "parent_run_id") == run_id);
	CHECK(daemon.RunValue(child, "status") == "succeeded");
	CHECK(daemon.RunValue(child, "workbook_id") == WORKBOOK);
	CHECK(daemon.RunValue(child, "params_json") == "{\"bu\":\"204\"}");
	CHECK(script.Requests() > 0);
	CHECK(Contains(script.sql.front(), "org_id = 204"));

	const auto key = daemon.RunValue(run_id, "instance_key");
	CHECK(key == ofexcel::InstanceKey("payment_reconciliation", ofexcel::DefinitionHash(RECONCILIATION), "local",
	                                  "{\"bu\":\"204\",\"tolerance\":\"0.01\"}"));
	CHECK(daemon.InstanceValue(key, "status") == "ready");
	const auto table = daemon.InstanceValue(key, "current_table");
	const auto rows = RunOk(*daemon.connection, "SELECT * FROM ds.\"" + table + "\" ORDER BY rowid");
	CHECK(rows.names == std::vector<std::string>({"invoice_num", "invoice_amount", "bank_amount", "diff"}));
	CHECK(rows.rows.size() == 2);
	CHECK(rows.At(0, 0).ToString() == "INV-2");
	CHECK(rows.At(0, 1).ToString() == "250.50");
	CHECK(rows.At(0, 2).ToString() == "200.00");
	CHECK(rows.At(0, 3).ToString() == "50.50");
	CHECK(rows.At(1, 0).ToString() == "INV-3");
	CHECK(rows.At(1, 2).IsNull());
	CHECK(rows.At(1, 3).ToString() == "75.75");
	const auto schema = RunOk(*daemon.connection, "SELECT column_name, contract_type FROM meta.query_schemas WHERE "
	                                              "definition_hash = '" +
	                                                  ofexcel::DefinitionHash(RECONCILIATION) + "' ORDER BY ordinal");
	CHECK(schema.rows.size() == 4);
	CHECK(schema.At(0, 1).ToString() == "string");
	CHECK(schema.At(1, 1).ToString() == "decimal(38,2)");
	CHECK(schema.At(2, 1).ToString() == "decimal(18,2)");
	CHECK(Contains(schema.At(3, 1).ToString(), "decimal("));

	// A ready dependency is not fetched again (deps = missing).
	script.Answer(Page(InvoiceRows()));
	const auto again = daemon.StartAndWait(daemon.Request("payment_reconciliation", {{"bu", Integer("204")}}));
	CHECK(daemon.RunValue(again, "status") == "succeeded");
	CHECK(daemon.Runs("query_id = 'ap_invoices'") == "1");
	CHECK(script.Requests() == 0);
	CHECK(daemon.InstanceValue(key, "current_version") == "2");
}

//! deps = all fetches a ready dependency again.
void TestDepsAll(Daemon &daemon) {
	script.Answer(Page(InvoiceRows()));
	const auto run_id = daemon.StartAndWait(
	    daemon.Request("payment_reconciliation", {{"bu", Integer("204")}}, ofexcel::DepsMode::ALL));
	CHECK(daemon.RunValue(run_id, "status") == "succeeded");
	CHECK(daemon.Runs("query_id = 'ap_invoices'") == "2");
	CHECK(daemon.Runs("query_id = 'ap_invoices' AND parent_run_id = '" + run_id + "'") == "1");
	CHECK(script.Requests() > 0);
}

//! deps = none runs nothing: a dependency without data is E_DEPENDENCY_MISSING
//! before any run exists, and the workbook's meta.missing says what to fetch
//! (§7.5) until a run of it succeeds.
void TestDepsNone(Daemon &daemon) {
	const auto runs = daemon.Runs("true");
	const auto request = daemon.Request("payment_reconciliation", {{"bu", Integer("999")}}, ofexcel::DepsMode::NONE);
	auto error = daemon.Refused(request);
	CHECK(error.code == "E_DEPENDENCY_MISSING");
	CHECK(error.status == 409);
	CHECK(Contains(error.what(), "ap_invoices"));
	CHECK(error.details);
	const auto details = ofexcel::JsonDocument::Parse(error.details->Serialize());
	const auto missing = details.Root().Get("missing");
	CHECK(missing.Size() == 1);
	CHECK(missing.At(0).Get("kind").String() == "query");
	CHECK(missing.At(0).Get("name").String() == "ap_invoices");
	CHECK(missing.At(0).Get("query").Get("hash").String() == ofexcel::DefinitionHash(INVOICES));
	CHECK(missing.At(0).Get("connection").String() == "fusion");
	CHECK(missing.At(0).Get("params").Get("bu").String() == "999");
	const auto key = missing.At(0).Get("instance_key").String();
	CHECK(key == ofexcel::InstanceKey("ap_invoices", ofexcel::DefinitionHash(INVOICES), "fusion", "{\"bu\":\"999\"}"));
	CHECK(daemon.Runs("true") == runs);

	const auto recorded = "SELECT requests FROM meta.missing WHERE workbook_id = '" + std::string(WORKBOOK) +
	                      "' AND instance_key = '" + key + "'";
	CHECK(daemon.Value(recorded) == "1");
	CHECK(daemon.Value("SELECT query_id || ' ' || connection_name || ' ' || params_json FROM meta.missing WHERE "
	                   "instance_key = '" +
	                   key + "'") == "ap_invoices fusion {\"bu\":\"999\"}");
	daemon.Refused(request);
	CHECK(daemon.Value(recorded) == "2");

	// Fetching the dependency clears the record, and the parent runs.
	script.Answer(Page(InvoiceRows()));
	const auto fetched = daemon.StartAndWait(daemon.Request("ap_invoices", {{"bu", Integer("999")}}));
	CHECK(daemon.RunValue(fetched, "instance_key") == key);
	CHECK(daemon.Value("SELECT count(*) FROM meta.missing WHERE instance_key = '" + key + "'") == "0");
	const auto run_id = daemon.StartAndWait(request);
	CHECK(daemon.RunValue(run_id, "status") == "succeeded");
	CHECK(daemon.Runs("parent_run_id = '" + run_id + "'") == "0");

	// An import cannot be fetched, whatever deps says.
	error = daemon.Refused(daemon.Request("needs_import"));
	CHECK(error.code == "E_DEPENDENCY_MISSING");
	CHECK(Contains(error.what(), "xl_import.absent"));
	CHECK(Contains(error.details->Serialize(), "\"kind\":\"import\""));
}

//! A dependency that fails fails its parent with E_DEPENDENCY_FAILED; the
//! parent's instance keeps no data.
void TestDependencyFails(Daemon &daemon) {
	script.Answer(Rejection("ORA-00942: table or view does not exist"));
	const auto run_id = daemon.StartAndWait(daemon.Request("payment_reconciliation", {{"bu", Integer("500")}}));
	CHECK(daemon.RunValue(run_id, "status") == "failed");
	CHECK(daemon.RunValue(run_id, "error_code") == "E_DEPENDENCY_FAILED");
	const auto message = daemon.RunValue(run_id, "error_message");
	CHECK(Contains(message, "ap_invoices"));
	CHECK(Contains(message, "E_FUSION_REJECTED"));
	CHECK(Contains(message, "ORA-00942"));
	const auto child = daemon.Value("SELECT run_id FROM meta.runs WHERE parent_run_id = '" + run_id + "'");
	CHECK(daemon.RunValue(child, "error_code") == "E_FUSION_REJECTED");
	const auto key = daemon.RunValue(run_id, "instance_key");
	CHECK(daemon.InstanceValue(key, "status") == "empty");
	CHECK(daemon.InstanceValue(key, "last_error_code") == "E_DEPENDENCY_FAILED");

	// Two Local queries that need each other: found when the second starts.
	const auto cycle = daemon.StartAndWait(daemon.Request("cycle_a"));
	CHECK(daemon.RunValue(cycle, "status") == "failed");
	CHECK(daemon.RunValue(cycle, "error_code") == "E_DEPENDENCY_FAILED");
	CHECK(Contains(daemon.RunValue(cycle, "error_message"), "cycle_a → cycle_b → cycle_a"));
}

//! §7: Local SQL that breaks a rule is refused before a run exists.
void TestValidation(Daemon &daemon) {
	const auto runs = daemon.Runs("true");
	const auto refuse = [&](const std::string &body, const std::string &code, const std::string &rule) {
		auto request = daemon.Request("ap_invoices");
		request.definition.text = Local("checked", "-- @param bu: integer = 1\n", body);
		request.definition.query_id = "checked";
		RunOk(*daemon.connection, "INSERT INTO meta.trusted_definitions VALUES ('" +
		                              ofexcel::DefinitionHash(request.definition.text) +
		                              "', 'checked', 'local_editor', NULL, TIMESTAMP '2026-01-01 00:00:00') "
		                              "ON CONFLICT DO NOTHING");
		const auto error = daemon.Refused(request);
		if (error.code != code) {
			std::cerr << "expected " << code << ", got " << error.code << ": " << error.what() << "\n";
		}
		CHECK(error.code == code);
		CHECK(error.status == 422);
		CHECK(Contains(error.what(), rule));
	};
	refuse("SELECT * FROM gl_balances", "E_VALIDATION", "V3");
	refuse("SELECT * FROM 'data.csv'", "E_VALIDATION", "V5");
	refuse("SELECT * FROM read_csv('x.csv')", "E_VALIDATION", "V6");
	refuse("SELECT getenv('HOME')", "E_VALIDATION", "V7");
	refuse("SELECT $other", "E_VALIDATION", "V8");
	refuse("SELEC 1", "E_SQL_COMPILE", "");
	CHECK(daemon.Runs("true") == runs);
	CHECK(daemon.Value("SELECT count(*) FROM meta.dataset_instances WHERE query_id = 'checked'") == "0");
}

//! A Local result stored as §3.2 stores its contract types (§3.4): integers by
//! name and size, '' kept apart from NULL, other types as strings with a
//! warning; parameters bound with their types; @max_rows.
void TestTypesAndValues(Daemon &daemon) {
	daemon.Register({INVOICES, RECONCILIATION, NEEDS_IMPORT, CYCLE_A, CYCLE_B,
	                 Local("typed",
	                       "-- @param ids: list<integer>\n-- @param big: integer = 123456789012345678901\n"
	                       "-- @param day: date = 2026-03-01\n-- @param unused: string = x\n",
	                       "SELECT x AS vendor_id, x * 10 AS n, $big AS huge, 1000000000000000 * x AS large,\n"
	                       "       CAST(x AS DOUBLE) / 4 AS ratio, $day + CAST(x AS INTEGER) AS d,\n"
	                       "       TIMESTAMPTZ '2026-01-01 10:00:00+00' AS stamp, CASE WHEN x = 1 THEN '' END AS s,\n"
	                       "       [x, x] AS pair, x > 1 AS flag\n"
	                       "FROM range(1, 4) AS t(x) WHERE list_contains($ids, x) ORDER BY x"),
	                 Local("limited", "-- @max_rows: 3\n", "SELECT x FROM range(10) AS t(x) ORDER BY x"),
	                 Local("declared", "-- @column code: integer | Code\n",
	                       "SELECT * FROM (VALUES ('7'), ('abc')) AS t(code)"),
	                 Local("bad_data", "", "SELECT CAST('x' AS INTEGER) AS n"),
	                 Local("bad_column", "", "SELECT nothing FROM range(3)"),
	                 Local("twice", "", "SELECT 1 AS a, 2 AS A")});

	const auto run_id = daemon.StartAndWait(daemon.Request(
	    "typed", {{"ids", ofexcel::ParamInput::Array({Integer("1"), Integer("3")})}}));
	if (daemon.RunValue(run_id, "status") != "succeeded") {
		std::cerr << daemon.RunValue(run_id, "error_code") << ": " << daemon.RunValue(run_id, "error_message") << "\n";
	}
	CHECK(daemon.RunValue(run_id, "status") == "succeeded");
	CHECK(daemon.RunValue(run_id, "rows_fetched") == "2");
	const auto warnings = daemon.RunValue(run_id, "warnings_json");
	CHECK(Contains(warnings, "{\"code\":\"W_PARAM_UNUSED\",\"message\":\"parameter unused is declared"));
	CHECK(Contains(warnings, "{\"code\":\"W_TYPE_WIDENED\",\"message\":\"column pair: type"));

	const auto table = daemon.RunValue(run_id, "result_table");
	const auto types = RunOk(*daemon.connection, "SELECT column_name, data_type FROM duckdb_columns() WHERE "
	                                             "schema_name = 'ds' AND table_name = '" +
	                                                 table + "' ORDER BY column_index");
	const std::vector<std::pair<std::string, std::string>> expected_types = {
	    {"vendor_id", "VARCHAR"}, {"n", "BIGINT"},         {"huge", "VARCHAR"}, {"large", "VARCHAR"},
	    {"ratio", "DOUBLE"},      {"d", "DATE"},           {"stamp", "TIMESTAMP"}, {"s", "VARCHAR"},
	    {"pair", "VARCHAR"},      {"flag", "BOOLEAN"}};
	CHECK(types.rows.size() == expected_types.size());
	for (size_t i = 0; i < expected_types.size(); i++) {
		CHECK(types.At(i, 0).ToString() == expected_types[i].first);
		if (types.At(i, 1).ToString() != expected_types[i].second) {
			std::cerr << expected_types[i].first << ": " << types.At(i, 1).ToString() << "\n";
		}
		CHECK(types.At(i, 1).ToString() == expected_types[i].second);
	}
	const auto schema = RunOk(*daemon.connection, "SELECT string_agg(contract_type, ' ' ORDER BY ordinal) FROM "
	                                              "meta.query_schemas WHERE definition_hash = '" +
	                                                  daemon.RunValue(run_id, "definition_hash") + "'");
	CHECK(schema.At(0, 0).ToString() == "id integer id id number date timestamptz string string boolean");

	const auto rows = RunOk(*daemon.connection, "SELECT * FROM ds.\"" + table + "\" ORDER BY rowid");
	CHECK(rows.rows.size() == 2);
	CHECK(rows.At(0, 0).ToString() == "1");
	CHECK(rows.At(1, 0).ToString() == "3");
	CHECK(rows.At(1, 1).ToString() == "30");
	CHECK(rows.At(0, 2).ToString() == "123456789012345678901");
	CHECK(rows.At(1, 3).ToString() == "3000000000000000");
	CHECK(rows.At(0, 4).ToString() == "0.25");
	CHECK(rows.At(1, 5).ToString() == "2026-03-04");
	CHECK(rows.At(0, 6).ToString() == "2026-01-01 10:00:00");
	CHECK(!rows.At(0, 7).IsNull());
	CHECK(rows.At(0, 7).ToString() == "");
	CHECK(rows.At(1, 7).IsNull());
	CHECK(rows.At(1, 8).ToString() == "[3, 3]");
	CHECK(rows.At(1, 9).ToString() == "true");

	const auto limited = daemon.StartAndWait(daemon.Request("limited"));
	CHECK(daemon.RunValue(limited, "status") == "succeeded");
	CHECK(daemon.RunValue(limited, "rows_fetched") == "3");
	CHECK(daemon.RunValue(limited, "truncated") == "true");

	// §3.3 item 1 for a Local result: the value must fit the @column type.
	const auto declared = daemon.StartAndWait(daemon.Request("declared"));
	CHECK(daemon.RunValue(declared, "status") == "failed");
	CHECK(daemon.RunValue(declared, "error_code") == "E_TYPE_DECLARED_MISMATCH");
	CHECK(daemon.RunValue(declared, "error_message") ==
	      "column code is declared as integer, but in row 2 the value is \"abc\"");

	// §14.1 for Local SQL.
	const auto bad_data = daemon.StartAndWait(daemon.Request("bad_data"));
	CHECK(daemon.RunValue(bad_data, "error_code") == "E_SQL_DATA");
	const auto bad_column = daemon.StartAndWait(daemon.Request("bad_column"));
	CHECK(daemon.RunValue(bad_column, "error_code") == "E_SQL_COMPILE");
	const auto twice = daemon.StartAndWait(daemon.Request("twice"));
	CHECK(daemon.RunValue(twice, "error_code") == "E_SQL_COMPILE");
	CHECK(Contains(daemon.RunValue(twice, "error_message"), "two columns named A"));
	CHECK(daemon.Value("SELECT count(*) FROM duckdb_tables() WHERE schema_name = 'ds' AND "
	                   "starts_with(table_name, 'stage__')") == "0");

	// ofexcel_run takes the same path.
	const auto sql = RunOk(*daemon.connection, "SELECT status, rows_fetched FROM ofexcel_run('limited')");
	CHECK(sql.At(0, 0).ToString() == "succeeded");
	CHECK(sql.At(0, 1).ToString() == "3");
}

//! Cancelling a run that waits for its dependency cancels the dependency it
//! started too.
void TestCancelChain(Daemon &daemon) {
	script.Answer(Page(InvoiceRows()), true);
	auto &engine = ofexcel::RunEngine::Get();
	const auto started = engine.Start(daemon.Db(), daemon.Request("payment_reconciliation", {{"bu", Integer("77")}}));
	WaitUntilHolding();
	CHECK(daemon.RunValue(started.run_id, "status") == "running");
	const auto child = daemon.Value("SELECT run_id FROM meta.runs WHERE parent_run_id = '" + started.run_id + "'");

	const auto cancelled_at = std::chrono::steady_clock::now();
	CHECK(engine.Cancel(started.run_id));
	engine.Wait(started.run_id);
	engine.Wait(child);
	CHECK(std::chrono::steady_clock::now() - cancelled_at < std::chrono::seconds(5));
	CHECK(daemon.RunValue(started.run_id, "status") == "cancelled");
	CHECK(daemon.RunValue(child, "status") == "cancelled");
	CHECK(engine.ActiveRuns() == 0);
}

} // namespace

int main() {
	test_support::TemporaryDirectory directory("ofexcel_local_runs_test");
	fs::create_directories(fs::u8path(directory.path));
	directory_path = directory.path;
	auto installed = ofquack::ScopedTransportFactory(
	    [](const ofquack::FusionConfig &) { return std::make_shared<FakeTransport>(); });

	{
		Daemon daemon("local");
		TestTypesAndValues(daemon);
		TestValidation(daemon);
		TestReconciliation(daemon);
		TestDepsAll(daemon);
		TestDepsNone(daemon);
		TestDependencyFails(daemon);
		TestCancelChain(daemon);
	}
	std::cout << "local_runs_test: all checks passed\n";
	return 0;
}
