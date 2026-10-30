// Local SQL validation (CONTRACTS.md §7): L1–L17 by their ids, then more
// cases per rule. Links DuckDB for its parser, opens no database.
//
// The query of §7's vectors: @depends: ap_invoices(...), @depends:
// xl_import.bank, @param bu: integer.

#include "ofexcel/local_validator.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

using namespace ofexcel;

namespace {

int failures = 0;

LocalSqlScope Scope() {
	return LocalSqlScope::FromLists({"ap_invoices(bu := $bu)", "xl_import.bank"}, {"bu"});
}

void Ok(const char *id, const std::string &sql) {
	const auto verdict = ValidateLocalSql(sql, Scope());
	if (!verdict.ok) {
		std::cerr << id << ": refused " << sql << "\n    " << verdict.code << " " << verdict.message << "\n";
		failures++;
	}
}

void Broken(const char *id, const std::string &sql, const char *rule, const std::string &fragment = "") {
	const auto verdict = ValidateLocalSql(sql, Scope());
	if (verdict.ok || verdict.code != "E_VALIDATION" || verdict.rule != rule ||
	    verdict.fragment.find(fragment) == std::string::npos || verdict.message.find(rule) == std::string::npos) {
		std::cerr << id << ": " << sql << "\n    expected " << rule << " \"" << fragment << "\", got "
		          << (verdict.ok ? "OK" : verdict.code + " " + verdict.rule + " \"" + verdict.fragment + "\" " + verdict.message)
		          << "\n";
		failures++;
	}
}

void TestVectors() {
	Ok("L1", "SELECT * FROM ap_invoices");
	Ok("L2", "SELECT * FROM ap_invoices i JOIN xl_import.bank b ON b.ref = i.invoice_num");
	Ok("L3", "WITH t AS (SELECT * FROM ap_invoices) SELECT count(*) FROM t");
	Broken("L4", "SELECT * FROM ap_invoices; SELECT 1", "V1");
	Broken("L5", "CREATE TABLE x AS SELECT 1", "V2");
	Broken("L6", "SELECT * FROM gl_balances", "V3", "gl_balances");
	Broken("L7", "SELECT * FROM xl_import.other", "V4", "xl_import.other");
	Broken("L8", "SELECT * FROM 'data.csv'", "V5", "data.csv");
	Broken("L9", "SELECT * FROM read_csv('x.csv')", "V6", "read_csv");
	Broken("L10", "SELECT * FROM oracle_fusion_query('SELECT 1 FROM dual')", "V6", "oracle_fusion_query");
	Broken("L11", "SELECT getenv('HOME')", "V7", "getenv");
	Ok("L12", "SELECT * FROM ap_invoices WHERE org_id = $bu");
	Broken("L13", "SELECT * FROM ap_invoices WHERE org_id = $x", "V8", "$x");
	Broken("L14", "SELECT * FROM ap_invoices WHERE org_id = ?", "V8");
	Ok("L15", "SELECT * FROM range(10)");
	Broken("L16", "SELECT * FROM (SELECT * FROM read_parquet('x'))", "V6", "read_parquet");
	Broken("L17", "SELECT * FROM memory.main.t", "V4", "memory.main.t");
}

void TestStatements() {
	Ok("V1 trailing semicolon", "SELECT 1;");
	Broken("V1 empty", "", "V1");
	Broken("V1 only a comment", "-- nothing", "V1");
	for (const auto *sql : {"INSERT INTO t VALUES (1)", "COPY (SELECT 1) TO 'x.csv'", "ATTACH 'x.db'", "SET threads = 1",
	                        "PRAGMA version", "INSTALL httpfs", "LOAD httpfs", "EXPLAIN SELECT 1",
	                        "CREATE MACRO m() AS 1", "DROP TABLE ap_invoices", "DELETE FROM ap_invoices"}) {
		Broken("V2 not a SELECT", sql, "V2");
	}
	Broken("V2 SUMMARIZE", "SUMMARIZE ap_invoices", "V2");
	Broken("V2 DESCRIBE in FROM", "SELECT * FROM (DESCRIBE ap_invoices)", "V2");
	Ok("V2 UNION", "SELECT 1 AS x UNION ALL SELECT 2 EXCEPT SELECT 3 INTERSECT SELECT 1");
	Ok("V2 VALUES", "SELECT * FROM (VALUES (1, 'a'), (2, 'b')) v(n, s)");
	Ok("V2 FROM-first", "FROM ap_invoices SELECT count(*)");
	const auto unparsable = ValidateLocalSql("SELEC 1", Scope());
	if (unparsable.ok || unparsable.code != "E_SQL_COMPILE" || !unparsable.rule.empty()) {
		std::cerr << "syntax error: expected E_SQL_COMPILE, got " << unparsable.code << " " << unparsable.rule << "\n";
		failures++;
	}
}

void TestTables() {
	Ok("V3 import without schema", "SELECT * FROM bank");
	Ok("V3 names in any case", "SELECT * FROM AP_Invoices JOIN XL_IMPORT.Bank USING (ref)");
	Ok("V3 CTE used by a later CTE", "WITH a AS (SELECT 1 AS x), b AS (SELECT * FROM a) SELECT * FROM b");
	Ok("V3 recursive CTE", "WITH RECURSIVE r(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM r WHERE n < 5) SELECT * FROM r");
	Ok("V3 CTE in a subquery", "SELECT * FROM (WITH t AS (SELECT 1 AS x) SELECT * FROM t)");
	Broken("V3 CTE is not visible outside its subquery",
	       "SELECT * FROM (WITH t AS (SELECT 1 AS x) SELECT * FROM t) s JOIN t USING (x)", "V3", "t");
	Broken("V3 in a JOIN", "SELECT * FROM ap_invoices JOIN gl_balances USING (id)", "V3", "gl_balances");
	Broken("V4 main schema", "SELECT * FROM main.ap_invoices", "V4", "main.ap_invoices");
	Broken("V4 meta", "SELECT * FROM meta.connections", "V4", "meta.connections");
	Broken("V4 catalog on an import", "SELECT * FROM data.xl_import.bank", "V4");
	Broken("V5 quoted identifier with a dot", "SELECT * FROM \"data.csv\"", "V5");
	Broken("V5 URL", "SELECT * FROM 'https://example.com/x.parquet'", "V5");
	Broken("V5 path", "SELECT * FROM '/etc/passwd'", "V5");
}

void TestFunctions() {
	Ok("V6 unnest", "SELECT * FROM unnest([1, 2, 3])");
	Ok("V6 generate_series", "SELECT * FROM generate_series(1, 3)");
	for (const auto *function : {"glob('*')", "query('SELECT 1')", "query_table('t')", "read_json('x')",
	                             "duckdb_settings()", "fusion_scanner_sso_login()", "main.range(3)"}) {
		Broken("V6 not allowed", std::string("SELECT * FROM ") + function, "V6");
	}
	Broken("V6 in a CTE", "WITH t AS (SELECT * FROM read_csv('x')) SELECT * FROM t", "V6", "read_csv");
	Broken("V6 in a WHERE subquery", "SELECT 1 FROM ap_invoices WHERE id IN (SELECT id FROM read_csv('x'))", "V6");
	for (const auto *call : {"current_setting('threads')", "ofexcel_version()", "read_text('x')", "read_blob('x')",
	                         "OFEXCEL_VERSION()", "fusion_scanner_version()", "oracle_fusion_tables()"}) {
		Broken("V7 forbidden", std::string("SELECT ") + call, "V7");
	}
	Broken("V7 nested", "SELECT upper(coalesce(getenv('X'), 'a')) FROM ap_invoices", "V7", "getenv");
	Broken("V7 in ORDER BY", "SELECT * FROM ap_invoices ORDER BY getenv('X')", "V7");
	Broken("V7 in a table function's argument", "SELECT * FROM range(current_setting('threads')::INT)", "V7");
	Broken("V7 in VALUES", "SELECT * FROM (VALUES (getenv('X')))", "V7");
	Broken("V7 in a scalar subquery", "SELECT (SELECT getenv('X'))", "V7");
	Ok("V7 ordinary functions", "SELECT upper(name), sum(amount), strftime(d, '%Y') FROM ap_invoices GROUP BY ALL");
}

void TestParameters() {
	Ok("V8 in a subquery", "SELECT * FROM ap_invoices WHERE id IN (SELECT id FROM bank WHERE org_id = $bu)");
	Ok("V8 any case", "SELECT $BU");
	Broken("V8 $1", "SELECT * FROM ap_invoices WHERE org_id = $1", "V8", "$1");
	Broken("V8 in a CTE", "WITH t AS (SELECT $x) SELECT * FROM t", "V8", "$x");

	// What the editor lists as used: $name, lower case, once each, in order.
	auto verdict = ValidateLocalSql("SELECT $BU, $bu FROM ap_invoices WHERE x = $bu", Scope());
	if (!verdict.ok || verdict.params_in_text != std::vector<std::string> {"bu"}) {
		std::cerr << "params_in_text: one entry for $BU and $bu\n";
		failures++;
	}
	verdict = ValidateLocalSql("SELECT $bu, $x", Scope());
	if (verdict.ok || verdict.params_in_text != std::vector<std::string> {"bu", "x"}) {
		std::cerr << "params_in_text: names met up to the refusal\n";
		failures++;
	}
	verdict = ValidateLocalSql("SELECT $1", Scope());
	if (verdict.ok || !verdict.params_in_text.empty()) {
		std::cerr << "params_in_text: never a positional parameter\n";
		failures++;
	}
}

} // namespace

int main() {
	TestVectors();
	TestStatements();
	TestTables();
	TestFunctions();
	TestParameters();
	if (failures) {
		std::cerr << failures << " validator check(s) failed\n";
		return EXIT_FAILURE;
	}
	std::cout << "local_validator_test: all checks passed\n";
	return EXIT_SUCCESS;
}
