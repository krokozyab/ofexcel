// Query file parsing (CONTRACTS.md §2). Pure: no DuckDB.
// P1–P16 are the vectors of §2.3, by their ids; D-<directive>-… cover each
// directive of §2.2 with a valid and an invalid value.

#include "ofexcel/query_file.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

using namespace ofexcel;

namespace {

int failures = 0;

const std::string HEAD = "-- @id: q\n-- @engine: oracle\n-- @version: 1\n";
const std::string LOCAL_HEAD = "-- @id: q\n-- @engine: local\n-- @version: 1\n";
const std::string BODY = "SELECT 1 FROM dual\n";

void Fail(const char *id, const std::string &what, const ParsedQueryFile &parsed) {
	std::cerr << id << ": " << what << "\n";
	for (const auto &error : parsed.errors) {
		std::cerr << "    " << error.code << " " << error.ToString() << "\n";
	}
	failures++;
}

//! Parses `text` and expects no error.
ParsedQueryFile Ok(const char *id, const std::string &text) {
	auto parsed = ParseQueryFile(text);
	if (!parsed.Ok()) {
		Fail(id, "expected OK", parsed);
	}
	return parsed;
}

//! Parses `text` and expects exactly one error with `code`, whose message
//! contains `fragment`, on `line` (-1: any line).
void Refused(const char *id, const std::string &text, const char *code, const std::string &fragment, int line = -1) {
	const auto parsed = ParseQueryFile(text);
	if (parsed.errors.size() != 1) {
		Fail(id, "expected exactly one error, got " + std::to_string(parsed.errors.size()), parsed);
		return;
	}
	const auto &error = parsed.errors[0];
	if (error.code != code || error.message.find(fragment) == std::string::npos || (line >= 0 && error.line != line)) {
		Fail(id, std::string("expected ") + code + " containing \"" + fragment + "\" on line " + std::to_string(line),
		     parsed);
	}
}

void Check(const char *id, bool condition, const ParsedQueryFile &parsed) {
	if (!condition) {
		Fail(id, "check failed", parsed);
	}
}

const QueryParam &OnlyParam(const ParsedQueryFile &parsed) {
	static const QueryParam NONE;
	return parsed.header.params.size() == 1 ? parsed.header.params[0] : NONE;
}

// --- §2.3 ------------------------------------------------------------------

void TestVectors() {
	{
		const auto parsed = Ok("P1", "-- @id: ap_invoices\n-- @engine: oracle\n-- @version: 1\nSELECT 1 FROM dual");
		const auto &header = parsed.header;
		Check("P1", header.id == "ap_invoices" && header.engine == Engine::ORACLE && header.version == 1 &&
		                header.body == "SELECT 1 FROM dual" && header.body_line == 4,
		      parsed);
	}
	Refused("P2", "-- @id: ap_invoices\n-- @engine: oracle\n" + BODY, "E_QUERY_PARSE",
	        "missing required directive @version", 0);
	Refused("P3", HEAD + "-- @engnie: oracle\n" + BODY, "E_QUERY_PARSE", "unknown directive @engnie", 4);
	{
		const auto parsed = Ok("P4", HEAD + "-- @param bu: integer = 204 | Business unit\n" + BODY);
		const auto &param = OnlyParam(parsed);
		Check("P4", param.name == "bu" && param.type.kind == ParamKind::INTEGER && param.has_default &&
		                !param.default_value.is_list && param.default_value.scalar == "204" &&
		                param.description == "Business unit" && param.line == 4,
		      parsed);
	}
	Refused("P5", HEAD + "-- @param d: date = 2026-13-01\n" + BODY, "E_QUERY_PARSE", "2026-13-01", 4);
	{
		const auto parsed = Ok("P6", HEAD + "-- @param s: string = \"a | b\"\n" + BODY);
		const auto &param = OnlyParam(parsed);
		Check("P6", param.default_value.scalar == "a | b" && param.description.empty(), parsed);
	}
	{
		const auto parsed = Ok("P7", HEAD + "-- @param st: enum(OPEN,PAID,ALL) = ALL\n" + BODY);
		const auto &param = OnlyParam(parsed);
		Check("P7", param.type.kind == ParamKind::ENUM && param.type.variants.size() == 3 &&
		                param.default_value.scalar == "ALL",
		      parsed);
	}
	Refused("P8", HEAD + "-- @param st: enum(OPEN,PAID) = ALL\n" + BODY, "E_QUERY_PARSE", "is not one of", 4);
	Refused("P9", HEAD + "-- @param ids: list<enum(A,B)>\n" + BODY, "E_QUERY_PARSE", "enum and list in list", 4);
	{
		const auto parsed = Ok("P10", HEAD + "-- @column VENDOR_ID: id | Supplier\n" + BODY);
		Check("P10", parsed.header.columns.size() == 1 && parsed.header.columns[0].name == "VENDOR_ID" &&
		                 parsed.header.columns[0].type.kind == ColumnKind::ID &&
		                 parsed.header.columns[0].header == "Supplier",
		      parsed);
	}
	Refused("P11", HEAD + "-- @refresh: incremental(key = id, watermark = d)\n" + BODY, "E_UNSUPPORTED",
	        "incremental refresh", 4);
	Refused("P12", HEAD + "-- @param bu: integer\n-- @depends: ap_invoices(bu := $bu)\n" + BODY, "E_QUERY_PARSE",
	        "@depends is only allowed", 5);
	Refused("P13", LOCAL_HEAD + "-- @connection: fusion_prod\n" + BODY, "E_QUERY_PARSE", "@connection is only allowed",
	        4);
	Refused("P14", HEAD + "-- @id: other\n" + BODY, "E_QUERY_PARSE", "@id is repeated (first on line 1)", 4);
	Refused("P15", HEAD + "-- only a comment\n\n", "E_QUERY_PARSE", "no query text");
	// Parameter names are lower case only (§1): "BU" is not a name, so it can
	// never collide with "bu".
	Refused("P16", HEAD + "-- @param BU: integer\n-- @param bu: string\n" + BODY, "E_QUERY_PARSE",
	        "\"BU\" is not a valid name", 4);
	Refused("P16 same name twice", HEAD + "-- @param bu: integer\n-- @param bu: string\n" + BODY, "E_QUERY_PARSE",
	        "@param bu is repeated", 5);
}

// --- §2.1 structure ----------------------------------------------------------

void TestStructure() {
	{
		const auto text = "\xEF\xBB\xBF" "-- @id: q\r\n   -- @engine: oracle\r\n\r\n-- comment @id: x\r\n"
		                  "--@version:1\r\nSELECT 1\r\nFROM dual -- @id: not_header\r\n";
		const auto parsed = Ok("S1 BOM, CRLF, indent, blank line, comment, no spaces", text);
		Check("S1", parsed.header.id == "q" && parsed.header.version == 1 && parsed.header.body_line == 6 &&
		                parsed.header.body == "SELECT 1\nFROM dual -- @id: not_header\n",
		      parsed);
	}
	{
		// The header stops at the first line that is neither blank nor "--":
		// a directive after it is part of the body.
		const auto parsed = Ok("S2 directive in body", HEAD + BODY + "-- @id: other\n");
		Check("S2", parsed.header.id == "q", parsed);
	}
	Refused("S3 body of comments only", HEAD + "\n" + "/* x */ \n", "E_QUERY_PARSE", "no query text");
	Refused("S4 upper-case key", HEAD + "-- @Title: x\n" + BODY, "E_QUERY_PARSE", "in lowercase: @title", 4);
	{
		// The blank line after the last "\n" is header too, so the body would start on line 4.
		const auto parsed = ParseQueryFile("-- @engnie: oracle\n-- @version: 0\n");
		Check("S5 every error reported, file-wide first",
		      parsed.errors.size() == 5 && parsed.errors[0].line == 0 && parsed.errors[1].line == 0 &&
		          parsed.errors[2].line == 1 && parsed.errors[3].line == 2 && parsed.errors[4].line == 4,
		      parsed);
	}
	Refused("S6 invalid UTF-8", HEAD + "-- @title: \xC3\x28\n" + BODY, "E_QUERY_PARSE", "UTF-8", 4);
	Refused("S7 empty value", HEAD + "-- @title:\n" + BODY, "E_QUERY_PARSE", "has no value", 4);
	{
		const auto parsed = Ok("S8 '-- @' without a letter is a comment", HEAD + "-- @ 10:00 daily\n" + BODY);
		Check("S8", parsed.header.title.empty(), parsed);
	}
}

// --- §2.2, one directive at a time -------------------------------------------

void TestId() {
	Ok("D-id-valid", "-- @id: ap_invoices_2\n-- @engine: oracle\n-- @version: 1\n" + BODY);
	Refused("D-id-invalid", "-- @id: AP-Invoices\n-- @engine: oracle\n-- @version: 1\n" + BODY, "E_QUERY_PARSE",
	        "is not an identifier", 1);
}

void TestEngine() {
	Check("D-engine-valid", Ok("D-engine-valid", LOCAL_HEAD + "SELECT 1\n").header.engine == Engine::LOCAL,
	      ParsedQueryFile());
	Refused("D-engine-invalid", "-- @id: q\n-- @engine: Oracle\n-- @version: 1\n" + BODY, "E_QUERY_PARSE",
	        "expected oracle or local", 2);
}

void TestVersion() {
	Check("D-version-valid",
	      Ok("D-version-valid", "-- @id: q\n-- @engine: oracle\n-- @version: 2147483647\n" + BODY).header.version ==
	          2147483647,
	      ParsedQueryFile());
	for (const auto *bad : {"0", "2147483648", "-1", "1.5", "v2"}) {
		Refused("D-version-invalid", "-- @id: q\n-- @engine: oracle\n-- @version: " + std::string(bad) + "\n" + BODY,
		        "E_QUERY_PARSE", "@version", 3);
	}
}

void TestTitle() {
	const std::string title_200(200, 'x');
	std::string non_ascii_200;
	for (int i = 0; i < 200; i++) {
		non_ascii_200 += "é";
	}
	Check("D-title-valid",
	      Ok("D-title-valid", HEAD + "-- @title: " + non_ascii_200 + "\n" + BODY).header.title == non_ascii_200,
	      ParsedQueryFile());
	Ok("D-title-valid ascii", HEAD + "-- @title: " + title_200 + "\n" + BODY);
	Refused("D-title-invalid", HEAD + "-- @title: " + non_ascii_200 + "é\n" + BODY, "E_QUERY_PARSE", "longer than 200",
	        4);
}

void TestDescription() {
	const auto parsed =
	    Ok("D-description-valid", HEAD + "-- @description: First\n-- @description:\n-- @description: Third\n" + BODY);
	Check("D-description-valid", parsed.header.description == "First\n\nThird", parsed);
	// Repeatable, so there is no invalid value; an unknown neighbour is the error.
	Refused("D-description-invalid", HEAD + "-- @descripton: x\n" + BODY, "E_QUERY_PARSE", "unknown directive", 4);
}

void TestConnection() {
	Check("D-connection-valid",
	      Ok("D-connection-valid", HEAD + "-- @connection: fusion_prod\n" + BODY).header.connection == "fusion_prod",
	      ParsedQueryFile());
	Refused("D-connection-invalid", HEAD + "-- @connection: Fusion Prod\n" + BODY, "E_QUERY_PARSE",
	        "is not a connection name", 4);
}

void TestParam() {
	{
		const auto parsed = Ok("D-param-valid", HEAD +
		                                            "-- @param a: decimal( 18 , 2 ) = 0012.500 | Amount\n"
		                                            "-- @param b: list<id> = 007, 8 ,9\n"
		                                            "-- @param c: list< date > = \"\"\n"
		                                            "-- @param d: timestamp = 2026-10-29T08:30:00.120000\n"
		                                            "-- @param e: boolean = Y\n"
		                                            "-- @param f: enum(Open, Paid) = paid\n"
		                                            "-- @param g: number = 0.10\n"
		                                            "-- @param h: string = \"say \\\"hi\\\"\" | quoted\n"
		                                            "-- @param i: string | no default value\n" +
		                                            BODY);
		const auto &params = parsed.header.params;
		Check("D-param-valid count and order",
		      params.size() == 9 && params[0].name == "a" && params[8].name == "i", parsed);
		if (params.size() == 9) {
			Check("D-param-valid decimal", params[0].type.ToString() == "decimal(18,2)" &&
			                                   params[0].default_value.scalar == "12.5" &&
			                                   params[0].description == "Amount",
			      parsed);
			Check("D-param-valid list", params[1].default_value.is_list &&
			                                (params[1].default_value.list == std::vector<std::string> {"7", "8", "9"}),
			      parsed);
			Check("D-param-valid empty list", params[2].default_value.is_list && params[2].default_value.list.empty() &&
			                                      params[2].type.ToString() == "list<date>",
			      parsed);
			Check("D-param-valid timestamp", params[3].default_value.scalar == "2026-10-29 08:30:00.12", parsed);
			Check("D-param-valid boolean", params[4].default_value.scalar == "true", parsed);
			Check("D-param-valid enum spelling", params[5].default_value.scalar == "Paid", parsed);
			Check("D-param-valid number", params[6].default_value.scalar == "0.1", parsed);
			Check("D-param-valid quoted", params[7].default_value.scalar == "say \"hi\"" && params[7].description == "quoted",
			      parsed);
			Check("D-param-valid no default", !params[8].has_default && params[8].description == "no default value",
			      parsed);
		}
	}
	Refused("D-param-invalid no colon", HEAD + "-- @param bu integer\n" + BODY, "E_QUERY_PARSE", "@param is written as",
	        4);
	Refused("D-param-invalid type", HEAD + "-- @param bu: int\n" + BODY, "E_QUERY_PARSE", "unknown type \"int\"", 4);
	Refused("D-param-invalid decimal", HEAD + "-- @param a: decimal(39,2)\n" + BODY, "E_QUERY_PARSE", "1 ≤ p ≤ 38", 4);
	Refused("D-param-invalid decimal fit", HEAD + "-- @param a: decimal(5,2) = 1234.5\n" + BODY, "E_QUERY_PARSE",
	        "does not fit", 4);
	Refused("D-param-invalid list element", HEAD + "-- @param b: list<id> = 1, x\n" + BODY, "E_QUERY_PARSE", "\"x\"",
	        4);
	Refused("D-param-invalid list of list", HEAD + "-- @param b: list<list<id>>\n" + BODY, "E_QUERY_PARSE", "in list",
	        4);
	Refused("D-param-invalid empty default", HEAD + "-- @param s: string = | x\n" + BODY, "E_QUERY_PARSE",
	        "no default value", 4);
	Refused("D-param-invalid unclosed quote", HEAD + "-- @param s: string = \"abc\n" + BODY, "E_QUERY_PARSE",
	        "unterminated", 4);
	Refused("D-param-invalid after quote", HEAD + "-- @param s: string = \"a\" b\n" + BODY, "E_QUERY_PARSE",
	        "expected \"|\"", 4);
	Refused("D-param-invalid enum duplicate", HEAD + "-- @param st: enum(A,a)\n" + BODY, "E_QUERY_PARSE", "is repeated",
	        4);
	Refused("D-param-invalid trailing", HEAD + "-- @param n: integer integer\n" + BODY, "E_QUERY_PARSE", "unexpected",
	        4);
	Refused("D-param-invalid number exponent", HEAD + "-- @param n: number = 1e5\n" + BODY, "E_QUERY_PARSE", "exponent",
	        4);
}

void TestColumn() {
	{
		const auto parsed = Ok("D-column-valid", HEAD +
		                                             "-- @column AMOUNT: decimal(18,2)\n"
		                                             "-- @column created$at#: timestamptz | Created\n" +
		                                             BODY);
		Check("D-column-valid", parsed.header.columns.size() == 2 &&
		                            parsed.header.columns[0].type.ToString() == "decimal(18,2)" &&
		                            parsed.header.columns[1].type.kind == ColumnKind::TIMESTAMPTZ,
		      parsed);
	}
	Refused("D-column-invalid type", HEAD + "-- @column AMOUNT: money\n" + BODY, "E_QUERY_PARSE", "unknown column type",
	        4);
	Refused("D-column-invalid name", HEAD + "-- @column 1ST: id\n" + BODY, "E_QUERY_PARSE",
	        "is not a valid column name", 4);
	Refused("D-column-invalid duplicate", HEAD + "-- @column vendor_id: id\n-- @column VENDOR_ID: id\n" + BODY,
	        "E_QUERY_PARSE", "is repeated", 5);
}

void TestDepends() {
	{
		const auto parsed = Ok("D-depends-valid", LOCAL_HEAD +
		                                              "-- @param bu: integer\n"
		                                              "-- @depends: ap_invoices(bu := $bu, s := 'it''s', n := -1.5, "
		                                              "d := date '2026-01-31', f := TRUE)\n"
		                                              "-- @depends: xl_import.budget\n"
		                                              "-- @depends: vendors()\n"
		                                              "SELECT 1\n");
		const auto &depends = parsed.header.depends;
		Check("D-depends-valid count", depends.size() == 3, parsed);
		if (depends.size() == 3) {
			const auto &args = depends[0].arguments;
			Check("D-depends-valid query", depends[0].kind == Dependency::Kind::QUERY && depends[0].name == "ap_invoices" &&
			                                   args.size() == 5,
			      parsed);
			if (args.size() == 5) {
				Check("D-depends-valid $param", args[0].kind == DependencyArgument::Kind::PARAM && args[0].value == "bu",
				      parsed);
				Check("D-depends-valid string", args[1].kind == DependencyArgument::Kind::STRING && args[1].value == "it's",
				      parsed);
				Check("D-depends-valid number", args[2].kind == DependencyArgument::Kind::NUMBER && args[2].value == "-1.5",
				      parsed);
				Check("D-depends-valid date", args[3].kind == DependencyArgument::Kind::DATE && args[3].value == "2026-01-31",
				      parsed);
				Check("D-depends-valid boolean", args[4].kind == DependencyArgument::Kind::BOOLEAN && args[4].value == "true",
				      parsed);
			}
			Check("D-depends-valid import", depends[1].kind == Dependency::Kind::IMPORT && depends[1].name == "budget",
			      parsed);
			Check("D-depends-valid no arguments", depends[2].arguments.empty(), parsed);
		}
	}
	Refused("D-depends-invalid syntax", LOCAL_HEAD + "-- @depends: ap_invoices(bu = 1)\n" + BODY, "E_QUERY_PARSE",
	        "\":=\"", 4);
	Refused("D-depends-invalid undeclared", LOCAL_HEAD + "-- @depends: ap_invoices(bu := $bu)\n" + BODY, "E_QUERY_PARSE",
	        "$bu is not declared", 4);
	Refused("D-depends-invalid date", LOCAL_HEAD + "-- @depends: ap_invoices(d := DATE '2026-02-30')\n" + BODY,
	        "E_QUERY_PARSE", "2026-02-30", 4);
	Refused("D-depends-invalid unclosed", LOCAL_HEAD + "-- @depends: ap_invoices(s := 'x)\n" + BODY, "E_QUERY_PARSE",
	        "unterminated string", 4);
	Refused("D-depends-invalid self", LOCAL_HEAD + "-- @depends: q()\n" + BODY, "E_QUERY_PARSE", "depend on itself", 4);
	Refused("D-depends-invalid repeated argument", LOCAL_HEAD + "-- @depends: a(x := 1, x := 2)\n" + BODY, "E_QUERY_PARSE",
	        "is repeated", 4);
	Refused("D-depends-invalid trailing", LOCAL_HEAD + "-- @depends: a() b\n" + BODY, "E_QUERY_PARSE",
	        "unexpected text", 4);
}

void TestRefresh() {
	Ok("D-refresh-valid", HEAD + "-- @refresh: full\n" + BODY);
	Refused("D-refresh-invalid", HEAD + "-- @refresh: daily\n" + BODY, "E_QUERY_PARSE", "expected full", 4);
}

void TestMaxRows() {
	Check("D-max_rows-valid", Ok("D-max_rows-valid", HEAD + "-- @max_rows: 1048575\n" + BODY).header.max_rows == 1048575,
	      ParsedQueryFile());
	Refused("D-max_rows-invalid", HEAD + "-- @max_rows: 1048576\n" + BODY, "E_QUERY_PARSE", "from 1 to 1048575", 4);
}

} // namespace

int main() {
	TestVectors();
	TestStructure();
	TestId();
	TestEngine();
	TestVersion();
	TestTitle();
	TestDescription();
	TestConnection();
	TestParam();
	TestColumn();
	TestDepends();
	TestRefresh();
	TestMaxRows();
	if (failures) {
		std::cerr << failures << " query file check(s) failed\n";
		return EXIT_FAILURE;
	}
	std::cout << "query_file_test: all checks passed\n";
	return EXIT_SUCCESS;
}
