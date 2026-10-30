// Oracle SQL parameter substitution (CONTRACTS.md §4): R1–R30 by their ids,
// lists at the 999-element chunk boundaries, q-literals with every bracket,
// quoted identifiers with a doubled quote. Pure: no DuckDB.
//
// §4.3's parameters: bu: integer, n: string, d: date, ts: timestamp,
// a: decimal(18,2), ids: list<integer>, f: boolean, bu_2: integer. Each has a
// default here, so that a vector passes only the values it is about; R19 uses
// a header where bu has none.

#include "ofexcel/oracle_render.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>

using namespace ofexcel;

namespace {

int failures = 0;

std::vector<QueryParam> Params(bool bu_default = true) {
	const std::string text = std::string("-- @id: q\n-- @engine: oracle\n-- @version: 1\n") +
	                         (bu_default ? "-- @param bu: integer = 1\n" : "-- @param bu: integer\n") +
	                         "-- @param n: string = \"x\"\n"
	                         "-- @param d: date = 2026-01-01\n"
	                         "-- @param ts: timestamp = 2026-01-01 00:00:00\n"
	                         "-- @param a: decimal(18,2) = 0\n"
	                         "-- @param ids: list<integer> = \"\"\n"
	                         "-- @param f: boolean = N\n"
	                         "-- @param bu_2: integer = 0\n"
	                         "SELECT 1 FROM dual\n";
	const auto parsed = ParseQueryFile(text);
	if (!parsed.Ok()) {
		std::cerr << "test header does not parse: " << parsed.errors[0].ToString() << "\n";
		std::abort();
	}
	return parsed.header.params;
}

using Inputs = std::map<std::string, ParamInput>;

ParamInput N(const std::string &number) {
	return ParamInput::Number(number);
}

ParamInput S(const std::string &text) {
	return ParamInput::String(text);
}

ParamInput NumberList(size_t count, size_t first = 1) {
	std::vector<ParamInput> elements;
	for (size_t i = 0; i < count; i++) {
		elements.push_back(N(std::to_string(first + i)));
	}
	return ParamInput::Array(std::move(elements));
}

//! A fragment that does not start a statement is put after "SELECT * FROM t ".
std::string Statement(const std::string &fragment) {
	const auto upper = fragment.substr(0, 6);
	if (upper == "SELECT" || fragment.compare(0, 2, "--") == 0 || fragment.compare(0, 2, "/*") == 0 ||
	    upper == "UPDATE" || fragment.compare(0, 1, "(") == 0) {
		return fragment;
	}
	return "SELECT * FROM t " + fragment;
}

void Renders(const char *id, const std::string &fragment, const Inputs &inputs, const std::string &expected,
             const std::vector<QueryParam> &params = Params()) {
	const auto result = RenderOracle(Statement(fragment), params, inputs);
	if (!result.ok) {
		std::cerr << id << ": refused: " << result.code << " " << result.message << "\n";
		failures++;
	} else if (result.rendered != Statement(expected)) {
		std::cerr << id << ":\n  got      " << result.rendered << "\n  expected " << Statement(expected) << "\n";
		failures++;
	}
}

void Refused(const char *id, const std::string &fragment, const Inputs &inputs, const char *code,
             const std::string &fragment_of_message = "", const std::vector<QueryParam> &params = Params()) {
	const auto result = RenderOracle(Statement(fragment), params, inputs);
	if (result.ok || result.code != code || result.message.find(fragment_of_message) == std::string::npos) {
		std::cerr << id << ": expected " << code << " \"" << fragment_of_message << "\", got "
		          << (result.ok ? "OK: " + result.rendered : result.code + " " + result.message) << "\n";
		failures++;
	}
}

//! The subquery for 1..count, built independently of OracleLiteral's loop.
std::string ExpectedList(size_t count) {
	std::string expected = "(";
	for (size_t chunk = 0; chunk * 999 < count; chunk++) {
		expected += chunk ? " UNION ALL " : "";
		expected += "SELECT column_value FROM TABLE(sys.odcinumberlist(";
		for (size_t i = chunk * 999; i < std::min(count, chunk * 999 + 999); i++) {
			expected += (i > chunk * 999 ? ", " : "") + std::to_string(i + 1);
		}
		expected += "))";
	}
	return expected + ")";
}

size_t Count(const std::string &text, const std::string &needle) {
	size_t count = 0;
	for (auto at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) {
		count++;
	}
	return count;
}

void TestVectors() {
	Renders("R1", "WHERE org_id = :bu", {{"bu", N("204")}}, "WHERE org_id = 204");
	Renders("R2", "WHERE name = :n", {{"n", S("O'Brien")}}, "WHERE name = 'O''Brien'");
	Renders("R3", "WHERE x = ':bu' AND y = :bu", {{"bu", N("204")}}, "WHERE x = ':bu' AND y = 204");
	Renders("R4", "-- :bu\nSELECT :bu FROM dual", {{"bu", N("204")}}, "-- :bu\nSELECT 204 FROM dual");
	Renders("R5", "/* :bu */ SELECT 1 FROM dual WHERE a = :bu", {{"bu", N("204")}},
	        "/* :bu */ SELECT 1 FROM dual WHERE a = 204");
	Renders("R6", "SELECT q'[it's :bu]' FROM dual WHERE a = :bu", {{"bu", N("204")}},
	        "SELECT q'[it's :bu]' FROM dual WHERE a = 204");
	Renders("R7", "SELECT nq'{:bu}' FROM dual", {}, "SELECT nq'{:bu}' FROM dual");
	Renders("R8", "SELECT \"COL:bu\" FROM t WHERE a = :BU", {{"bu", N("204")}}, "SELECT \"COL:bu\" FROM t WHERE a = 204");
	Renders("R9", "WHERE d >= :d", {{"d", S("2026-01-01")}}, "WHERE d >= DATE '2026-01-01'");
	Renders("R10", "WHERE t <= :ts", {{"ts", S("2026-03-31T23:59:59")}}, "WHERE t <= TIMESTAMP '2026-03-31 23:59:59'");
	Renders("R11", "WHERE amt > :a", {{"a", N("-12.5")}}, "WHERE amt > (-12.5)");
	Renders("R12", "WHERE x - :bu > 0", {{"bu", N("-5")}}, "WHERE x - (-5) > 0");
	Renders("R13", "WHERE id IN :ids", {{"ids", NumberList(3)}},
	        "WHERE id IN (SELECT column_value FROM TABLE(sys.odcinumberlist(1, 2, 3)))");
	{
		const auto result = RenderOracle(Statement("WHERE id IN :ids"), Params(), {{"ids", NumberList(1500)}});
		const bool shaped = result.ok && result.rendered == Statement("WHERE id IN " + ExpectedList(1500)) &&
		                    Count(result.rendered, "TABLE(sys.odcinumberlist(") == 2 &&
		                    Count(result.rendered, " UNION ALL ") == 1 &&
		                    result.rendered.find("999)) UNION ALL SELECT column_value FROM TABLE(sys.odcinumberlist(1000, ") !=
		                        std::string::npos &&
		                    result.rendered.find(", 1500)))") != std::string::npos;
		if (!shaped) {
			std::cerr << "R14: 1500 elements are not 999 + 501 joined by UNION ALL\n";
			failures++;
		}
	}
	Renders("R15", "WHERE id IN :ids", {{"ids", ParamInput::Array({})}}, "WHERE id IN (SELECT NULL FROM dual WHERE 1 = 0)");
	Refused("R16", "WHERE id = :ids", {{"ids", NumberList(1)}}, "E_PARAM_LIST_CONTEXT", ":ids");
	Refused("R17", "WHERE a = :x", {}, "E_PARAM_UNKNOWN", ":x");
	Refused("R18", "WHERE a = :1", {}, "E_PARAM_POSITIONAL", ":1");
	Refused("R19", "WHERE a = :bu", {}, "E_PARAM_MISSING", "bu", Params(false));
	Refused("R20", "WHERE a = :n", {{"n", S(std::string("a\0b", 3))}}, "E_PARAM_TYPE", "n");
	Refused("R21", "WHERE a = :n", {{"n", S(std::string(4001, 'x'))}}, "E_PARAM_TOO_LONG", "n");
	Renders("R22", "WHERE a = :bu_2", {{"bu", N("1")}, {"bu_2", N("2")}}, "WHERE a = 2");
	Renders("R23", "SELECT :bu||'x' FROM dual", {{"bu", N("204")}}, "SELECT 204||'x' FROM dual");
	Refused("R24", "WHERE a = :bu", {{"bu", N("3.5")}}, "E_PARAM_TYPE", "bu");
	Renders("R25", "WHERE a = :bu", {{"bu", N("3.0")}}, "WHERE a = 3");
	Renders("R26", "WHERE flag = :f", {{"f", ParamInput::Boolean(true)}}, "WHERE flag = 'Y'");
	Refused("R27", "SELECT 'abc FROM dual", {}, "E_SQL_TOKENIZE");
	Refused("R28", "UPDATE t SET a = 1", {}, "E_SQL_NOT_SELECT");
	Renders("R29", "/* c */ WITH t AS (SELECT :bu b FROM dual) SELECT * FROM t", {{"bu", N("204")}},
	        "/* c */ WITH t AS (SELECT 204 b FROM dual) SELECT * FROM t");
	Refused("R30", "WHERE a = :x::y", {}, "E_PARAM_UNKNOWN", ":x");
	Renders("R30 :: copied", "WHERE a = :bu::y", {{"bu", N("7")}}, "WHERE a = 7::y");
}

void TestListBoundaries() {
	for (const size_t count : {size_t(999), size_t(1000), size_t(1998), size_t(1999)}) {
		const auto result = RenderOracle(Statement("WHERE id IN :ids"), Params(), {{"ids", NumberList(count)}});
		const size_t chunks = (count + 998) / 999;
		if (!result.ok || result.rendered != Statement("WHERE id IN " + ExpectedList(count)) ||
		    Count(result.rendered, "TABLE(sys.odcinumberlist(") != chunks) {
			std::cerr << "list of " << count << ": expected " << chunks << " chunk(s)\n";
			failures++;
		}
	}
	// Each chunk ends at the 999th element: 1000 is one full chunk and one of a single element.
	const auto thousand = RenderOracle(Statement("WHERE id IN :ids"), Params(), {{"ids", NumberList(1000)}});
	if (thousand.rendered.find("odcinumberlist(1000))") == std::string::npos) {
		std::cerr << "list of 1000: the second chunk is not (1000)\n";
		failures++;
	}
}

void TestListLiterals() {
	const auto params = ParseQueryFile("-- @id: q\n-- @engine: oracle\n-- @version: 1\n"
	                                   "-- @param s: list<string>\n-- @param d: list<date>\n"
	                                   "-- @param t: list<timestamp>\n-- @param b: list<boolean>\n"
	                                   "-- @param x: list<decimal(10,2)>\nSELECT 1 FROM dual\n")
	                        .header.params;
	Renders("list<string>", "WHERE a IN :s", {{"s", ParamInput::Array({S("O'B"), S("x")})}, {"d", ParamInput::Array({})},
	                                          {"t", ParamInput::Array({})}, {"b", ParamInput::Array({})},
	                                          {"x", ParamInput::Array({})}},
	        "WHERE a IN (SELECT column_value FROM TABLE(sys.odcivarchar2list('O''B', 'x')))", params);
	const Inputs dates = {{"s", ParamInput::Array({})},
	                      {"d", ParamInput::Array({S("2026-01-01")})},
	                      {"t", ParamInput::Array({S("2026-01-01 10:00:00.5")})},
	                      {"b", ParamInput::Array({ParamInput::Boolean(true), S("N")})},
	                      {"x", ParamInput::Array({N("-1.5"), S("2")})}};
	Renders("list<date>", "WHERE a IN :d", dates,
	        "WHERE a IN (SELECT column_value FROM TABLE(sys.odcidatelist(DATE '2026-01-01')))", params);
	Renders("list<timestamp>", "WHERE a IN :t", dates,
	        "WHERE a IN (SELECT column_value FROM TABLE(sys.odcidatelist(TIMESTAMP '2026-01-01 10:00:00.5')))", params);
	Renders("list<boolean>", "WHERE a IN :b", dates,
	        "WHERE a IN (SELECT column_value FROM TABLE(sys.odcivarchar2list('Y', 'N')))", params);
	Renders("list<decimal>", "WHERE a IN :x", dates,
	        "WHERE a IN (SELECT column_value FROM TABLE(sys.odcinumberlist((-1.5), 2)))", params);
}

void TestTokenizer() {
	for (const auto *q : {"q'<it's :bu>'", "q'{it's :bu}'", "q'(it's :bu)'", "Q'!it's :bu!'", "Nq'[:bu]'", "NQ'#:bu#'"}) {
		const std::string fragment = std::string("SELECT ") + q + " FROM dual WHERE a = :bu";
		Renders("q-literal", fragment, {{"bu", N("5")}}, std::string("SELECT ") + q + " FROM dual WHERE a = 5");
	}
	Renders("q-literal ends at bracket + quote only", "SELECT q'[a]b]' FROM dual WHERE a = :bu", {{"bu", N("5")}},
	        "SELECT q'[a]b]' FROM dual WHERE a = 5");
	Refused("q-literal unclosed", "SELECT q'[abc' FROM dual", {}, "E_SQL_TOKENIZE", "q-literal");
	Refused("q-literal without delimiter", "SELECT q' x' FROM dual", {}, "E_SQL_TOKENIZE");
	Renders("quoted identifier with \"\"", "SELECT \"A\"\"B:bu\" FROM t WHERE a = :bu", {{"bu", N("5")}},
	        "SELECT \"A\"\"B:bu\" FROM t WHERE a = 5");
	Refused("quoted identifier unclosed", "SELECT \"A FROM t", {}, "E_SQL_TOKENIZE", "identifier");
	Renders("doubled quote in a string", "SELECT 'it''s :bu' FROM dual WHERE a = :bu", {{"bu", N("5")}},
	        "SELECT 'it''s :bu' FROM dual WHERE a = 5");
	Renders("N'…' string", "SELECT N':bu' FROM dual WHERE a = :bu", {{"bu", N("5")}},
	        "SELECT N':bu' FROM dual WHERE a = 5");
	Renders("word ending in q is not a q-literal", "SELECT colq FROM t WHERE a = :bu", {{"bu", N("5")}},
	        "SELECT colq FROM t WHERE a = 5");
	Refused("block comment unclosed", "SELECT 1 FROM dual /* :bu", {}, "E_SQL_TOKENIZE", "/*");
	Renders("line comment at the end", "SELECT :bu FROM dual -- end", {{"bu", N("5")}}, "SELECT 5 FROM dual -- end");
	Renders("colon before a space", "SELECT CASE WHEN a = : bu THEN 1 END FROM dual", {}, "SELECT CASE WHEN a = : bu THEN 1 END FROM dual");
	Refused("reserved __ name", "WHERE a = :__run", {}, "E_PARAM_UNKNOWN", "__");
	Renders("IN across a comment", "WHERE id NOT IN /* c */ :ids", {{"ids", NumberList(1)}},
	        "WHERE id NOT IN /* c */ (SELECT column_value FROM TABLE(sys.odcinumberlist(1)))");
	Renders("in, lower case", "WHERE id in\n  :ids", {{"ids", NumberList(1)}},
	        "WHERE id in\n  (SELECT column_value FROM TABLE(sys.odcinumberlist(1)))");
	Refused("list after (", "WHERE id IN (:ids)", {{"ids", NumberList(1)}}, "E_PARAM_LIST_CONTEXT");
	Refused("list after a word ending in IN", "WHERE id JOIN :ids", {{"ids", NumberList(1)}}, "E_PARAM_LIST_CONTEXT");
	Refused("SELECT must lead", "(SELECT 1 FROM dual)", {}, "E_SQL_NOT_SELECT");
	Refused("SELECTED is not SELECT", "SELECTED 1", {}, "E_SQL_NOT_SELECT");
	Refused("error names its line", "SELECT 1\nFROM dual\nWHERE a = :1", {}, "E_PARAM_POSITIONAL", "line 3");
	Refused("undeclared input", "WHERE a = :bu", {{"zz", N("1")}}, "E_PARAM_UNKNOWN", "zz");
	Renders("input names in any case", "WHERE a = :bu", {{"BU", N("9")}}, "WHERE a = 9");
	Renders("string with newline", "WHERE a = :n", {{"n", S("a\nb")}}, "WHERE a = 'a\nb'");
}

void TestValuesAndWarnings() {
	const auto result = RenderOracle("SELECT :bu FROM dual", Params(), {{"bu", N("204")}});
	const bool values = result.ok && result.values.size() == 8 && result.values.at("bu").scalar == "204" &&
	                    result.values.at("n").scalar == "x" && result.values.at("ids").is_list &&
	                    result.values.at("ids").list.empty();
	if (!values) {
		std::cerr << "values: every declared parameter, defaults applied\n";
		failures++;
	}
	if (result.warnings.size() != 7 || result.warnings[0].find("n") == std::string::npos) {
		std::cerr << "warnings: one per declared parameter the text does not use, got " << result.warnings.size()
		          << "\n";
		failures++;
	}
}

void TestCheckText() {
	// What the editor sees without values: the names used, in order, once each.
	auto check = CheckOracleText("SELECT :BU, :n FROM t WHERE x IN :ids AND y = :bu", Params());
	if (!check.ok || check.params_in_text != std::vector<std::string> {"bu", "n", "ids"} || check.warnings.size() != 5) {
		std::cerr << "check text: names used and warnings for the rest\n";
		failures++;
	}
	// A missing value is not the text's problem.
	check = CheckOracleText("SELECT :bu FROM dual", Params(false));
	if (!check.ok) {
		std::cerr << "check text: no value needed\n";
		failures++;
	}
	check = CheckOracleText("SELECT :bu, :nope FROM dual", Params());
	if (check.ok || check.code != "E_PARAM_UNKNOWN" || check.params_in_text != std::vector<std::string> {"bu", "nope"}) {
		std::cerr << "check text: an undeclared name is refused and still listed\n";
		failures++;
	}
	for (const auto &refused : std::vector<std::pair<std::string, std::string>> {
	         {"SELECT 'x FROM dual", "E_SQL_TOKENIZE"},
	         {"DELETE FROM t", "E_SQL_NOT_SELECT"},
	         {"SELECT :1 FROM dual", "E_PARAM_POSITIONAL"},
	         {"SELECT * FROM t WHERE x = :ids", "E_PARAM_LIST_CONTEXT"}}) {
		check = CheckOracleText(refused.first, Params());
		if (check.ok || check.code != refused.second) {
			std::cerr << "check text: " << refused.first << " expected " << refused.second << ", got " << check.code
			          << "\n";
			failures++;
		}
	}
}

} // namespace

int main() {
	TestVectors();
	TestListBoundaries();
	TestListLiterals();
	TestTokenizer();
	TestValuesAndWarnings();
	TestCheckText();
	if (failures) {
		std::cerr << failures << " render check(s) failed\n";
		return EXIT_FAILURE;
	}
	std::cout << "oracle_render_test: all checks passed\n";
	return EXIT_SUCCESS;
}
