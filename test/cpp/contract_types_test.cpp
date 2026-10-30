// Parameter and column types and the canonical form of text values
// (CONTRACTS.md §2.2, §3.1). Pure: no DuckDB. One block per row of §3.1, each
// with accepted and refused inputs; JSON input is TASKS.md B3's.

#include "ofexcel/contract_types.hpp"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>

using namespace ofexcel;

namespace {

int failures = 0;

ParamType Type(const std::string &text) {
	ParamType type;
	std::string error;
	if (!ParseParamType(text, type, error)) {
		std::cerr << "cannot parse type " << text << ": " << error << "\n";
		failures++;
	}
	return type;
}

void Accepts(const char *id, const std::string &type, const std::string &input, const std::string &expected) {
	std::string canonical;
	std::string error;
	if (!CanonicalizeText(Type(type), input, canonical, error)) {
		std::cerr << id << ": " << type << " refused \"" << input << "\": " << error << "\n";
		failures++;
	} else if (canonical != expected) {
		std::cerr << id << ": " << type << " \"" << input << "\" → \"" << canonical << "\", expected \"" << expected
		          << "\"\n";
		failures++;
	}
}

void Refuses(const char *id, const std::string &type, const std::string &input) {
	std::string canonical;
	std::string error;
	if (CanonicalizeText(Type(type), input, canonical, error)) {
		std::cerr << id << ": " << type << " accepted \"" << input << "\" as \"" << canonical << "\"\n";
		failures++;
	} else if (error.empty()) {
		std::cerr << id << ": " << type << " refused \"" << input << "\" without a message\n";
		failures++;
	}
}

void Expect(const char *id, const std::string &actual, const std::string &expected) {
	if (actual != expected) {
		std::cerr << id << ": got \"" << actual << "\", expected \"" << expected << "\"\n";
		failures++;
	}
}

void TestInteger() {
	Accepts("integer", "integer", "204", "204");
	Accepts("integer", "integer", "007", "7");
	Accepts("integer", "integer", "-007", "-7");
	Accepts("integer", "integer", "-0", "0");
	Accepts("integer", "integer", std::string(38, '9'), std::string(38, '9'));
	Refuses("integer", "integer", std::string(39, '9'));
	Refuses("integer", "integer", "1.0");
	Refuses("integer", "integer", "1e3");
	Refuses("integer", "integer", "+1");
	Refuses("integer", "integer", "");
	Refuses("integer", "integer", "-");
}

void TestNumber() {
	Accepts("number", "number", "0.10", "0.1");
	Accepts("number", "number", ".5", "0.5");
	Accepts("number", "number", "5.", "5");
	Accepts("number", "number", "-0", "0");
	Accepts("number", "number", "-2.50", "-2.5");
	Accepts("number", "number", "9007199254740993", "9007199254740992");
	Accepts("number", "number", "100000000000000000000", "1e+20");
	Refuses("number", "number", "1e5");
	Refuses("number", "number", "NaN");
	Refuses("number", "number", "inf");
	Refuses("number", "number", "1" + std::string(400, '0'));
	Refuses("number", "number", ".");
	Refuses("number", "number", "");
}

void TestShortestDouble() {
	// What std::to_chars(double) writes for these.
	Expect("to_chars 0.1", ShortestDouble(0.1), "0.1");
	Expect("to_chars 1/3", ShortestDouble(1.0 / 3), "0.3333333333333333");
	Expect("to_chars 100", ShortestDouble(100), "100");
	Expect("to_chars 123456", ShortestDouble(123456), "123456");
	Expect("to_chars 1e20", ShortestDouble(1e20), "1e+20");
	Expect("to_chars 1e21", ShortestDouble(1e21), "1e+21");
	Expect("to_chars 1.5e300", ShortestDouble(1.5e300), "1.5e+300");
	Expect("to_chars 0.001 (tie, fixed)", ShortestDouble(0.001), "0.001");
	Expect("to_chars 0.0001", ShortestDouble(0.0001), "1e-04");
	Expect("to_chars 1.25e-7", ShortestDouble(1.25e-7), "1.25e-07");
	Expect("to_chars max", ShortestDouble(std::numeric_limits<double>::max()), "1.7976931348623157e+308");
	Expect("to_chars denorm min", ShortestDouble(std::numeric_limits<double>::denorm_min()), "5e-324");
	Expect("to_chars -2.5", ShortestDouble(-2.5), "-2.5");
	Expect("to_chars 2^53", ShortestDouble(9007199254740992.0), "9007199254740992");
	Expect("to_chars 0.3", ShortestDouble(0.1 + 0.2), "0.30000000000000004");
	Expect("-0 is 0", ShortestDouble(-0.0), "0");
}

void TestDecimal() {
	Accepts("decimal", "decimal(5,2)", "999.99", "999.99");
	Accepts("decimal", "decimal(5,2)", "1.230", "1.23");
	Accepts("decimal", "decimal(5,2)", "0012.500", "12.5");
	Accepts("decimal", "decimal(5,2)", "-0.00", "0");
	Accepts("decimal", "decimal(5,2)", ".5", "0.5");
	Accepts("decimal", "decimal(5,2)", "5.", "5");
	Accepts("decimal", "decimal(3,3)", "0.123", "0.123");
	Accepts("decimal", "decimal(38,0)", std::string(38, '9'), std::string(38, '9'));
	Refuses("decimal", "decimal(5,2)", "1234.5");
	Refuses("decimal", "decimal(5,2)", "1.234");
	Refuses("decimal", "decimal(3,3)", "1.1");
	Refuses("decimal", "decimal(5,2)", ".");
	Refuses("decimal", "decimal(5,2)", "1e2");
	Refuses("decimal", "decimal(5,2)", "");
}

void TestString() {
	Accepts("string", "string", "Zoë \"Z\"", "Zoë \"Z\"");
	Accepts("string", "string", "tab\tlf\ncr\r", "tab\tlf\ncr\r");
	Accepts("string", "string", "", "");
	Accepts("string", "string", std::string(4000, 'x'), std::string(4000, 'x'));
	Refuses("string", "string", std::string(4001, 'x'));
	Refuses("string", "string", "a\x01" "b");
	Refuses("string", "string", std::string("a\0b", 3));
	Refuses("string", "string", "\xC3\x28");
}

void TestId() {
	Accepts("id", "id", "300000047114611", "300000047114611");
	Accepts("id", "id", "0042", "42");
	Accepts("id", "id", "0", "0");
	Accepts("id", "id", std::string(38, '1'), std::string(38, '1'));
	Refuses("id", "id", std::string(39, '1'));
	Refuses("id", "id", "-1");
	Refuses("id", "id", "1.0");
	Refuses("id", "id", "");
}

void TestDate() {
	Accepts("date", "date", "2026-10-29", "2026-10-29");
	Accepts("date", "date", "2024-02-29", "2024-02-29");
	Accepts("date", "date", "2000-02-29", "2000-02-29");
	Refuses("date", "date", "2023-02-29");
	Refuses("date", "date", "1900-02-29");
	Refuses("date", "date", "2026-13-01");
	Refuses("date", "date", "2026-04-31");
	Refuses("date", "date", "2026-1-01");
	Refuses("date", "date", "0000-01-01");
	Refuses("date", "date", "2026-10-29T00:00:00");
	Refuses("date", "date", "29.10.2026");
}

void TestTimestamp() {
	Accepts("timestamp", "timestamp", "2026-10-29 08:30:00", "2026-10-29 08:30:00");
	Accepts("timestamp", "timestamp", "2026-10-29T08:30:00", "2026-10-29 08:30:00");
	Accepts("timestamp", "timestamp", "2026-10-29 08:30:00.000000", "2026-10-29 08:30:00");
	Accepts("timestamp", "timestamp", "2026-10-29 08:30:00.120", "2026-10-29 08:30:00.12");
	Accepts("timestamp", "timestamp", "2026-10-29 23:59:59.999999", "2026-10-29 23:59:59.999999");
	Refuses("timestamp", "timestamp", "2026-10-29 08:30:00.1234567");
	Refuses("timestamp", "timestamp", "2026-10-29 24:00:00");
	Refuses("timestamp", "timestamp", "2026-10-29 08:60:00");
	Refuses("timestamp", "timestamp", "2026-10-29 08:30");
	Refuses("timestamp", "timestamp", "2026-10-29");
	Refuses("timestamp", "timestamp", "2026-10-29 08:30:00.");
	Refuses("timestamp", "timestamp", "2026-10-29 08:30:00Z");
	Refuses("timestamp", "timestamp", "2026-02-30 08:30:00");
}

void TestBoolean() {
	for (const auto *yes : {"true", "TRUE", "True", "Y", "y", "1"}) {
		Accepts("boolean", "boolean", yes, "true");
	}
	for (const auto *no : {"false", "FALSE", "N", "n", "0"}) {
		Accepts("boolean", "boolean", no, "false");
	}
	Refuses("boolean", "boolean", "yes");
	Refuses("boolean", "boolean", "");
	Refuses("boolean", "boolean", "2");
}

void TestEnum() {
	Accepts("enum", "enum(Open,PAID,all)", "open", "Open");
	Accepts("enum", "enum(Open,PAID,all)", "PAID", "PAID");
	Accepts("enum", "enum(Open,PAID,all)", "ALL", "all");
	Refuses("enum", "enum(Open,PAID,all)", "closed");
	Refuses("enum", "enum(Open,PAID,all)", "");
}

void TestTypeGrammar() {
	Expect("type spaces", Type(" decimal ( 18 , 2 ) ").ToString(), "decimal(18,2)");
	Expect("type list", Type("list < id >").ToString(), "list<id>");
	Expect("type list decimal", Type("list<decimal(10,0)>").ToString(), "list<decimal(10,0)>");
	Expect("type enum", Type("enum( A , b_2 )").ToString(), "enum(A,b_2)");
	for (const auto *bad : {"", "Integer", "int", "decimal", "decimal(0,0)", "decimal(2,3)", "decimal(18;2)", "enum()",
	                        "enum(A,a)", "enum(A-B)", "list<id", "list<>", "list<list<id>>", "list<enum(A)>",
	                        "integer extra", "inte ger"}) {
		ParamType type;
		std::string error;
		if (ParseParamType(bad, type, error) || error.empty()) {
			std::cerr << "type grammar: accepted \"" << bad << "\"\n";
			failures++;
		}
	}
	for (const auto *good : {"id", "integer", "decimal(18,2)", "number", "string", "text", "date", "timestamp",
	                         "timestamptz", "boolean"}) {
		ColumnType type;
		std::string error;
		if (!ParseColumnType(good, type, error) || type.ToString() != good) {
			std::cerr << "column type: \"" << good << "\" → \"" << type.ToString() << "\" " << error << "\n";
			failures++;
		}
	}
	for (const auto *bad : {"enum(A)", "list<id>", "money", "Text", ""}) {
		ColumnType type;
		std::string error;
		if (ParseColumnType(bad, type, error)) {
			std::cerr << "column type: accepted \"" << bad << "\"\n";
			failures++;
		}
	}
}

} // namespace

int main() {
	TestInteger();
	TestNumber();
	TestShortestDouble();
	TestDecimal();
	TestString();
	TestId();
	TestDate();
	TestTimestamp();
	TestBoolean();
	TestEnum();
	TestTypeGrammar();
	if (failures) {
		std::cerr << failures << " contract type check(s) failed\n";
		return EXIT_FAILURE;
	}
	std::cout << "contract_types_test: all checks passed\n";
	return EXIT_SUCCESS;
}
