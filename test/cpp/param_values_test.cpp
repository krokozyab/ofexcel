// Parameter values as the add-in sends them, in JSON (CONTRACTS.md §3.1):
// a block per row of the table, accepted and refused. Text input on its own
// is covered by contract_types_test.cpp. Pure: no DuckDB.

#include "ofexcel/param_values.hpp"

#include <cstdlib>
#include <iostream>
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

std::string Describe(const CanonicalValue &value) {
	if (!value.is_list) {
		return value.scalar;
	}
	std::string text = "[";
	for (size_t i = 0; i < value.list.size(); i++) {
		text += (i ? "," : "") + value.list[i];
	}
	return text + "]";
}

void Accepts(const char *id, const std::string &type, const ParamInput &input, const std::string &expected,
             const CanonicalValue *default_value = nullptr) {
	const auto result = NormalizeParam(Type(type), default_value, input);
	if (!result.ok) {
		std::cerr << id << ": " << type << " refused «" << input.text << "»: " << result.code << " " << result.message
		          << "\n";
		failures++;
	} else if (Describe(result.value) != expected) {
		std::cerr << id << ": " << type << " «" << input.text << "» → «" << Describe(result.value) << "», expected «"
		          << expected << "»\n";
		failures++;
	}
}

void Refuses(const char *id, const std::string &type, const ParamInput &input, const char *code = "E_PARAM_TYPE",
             const std::string &fragment = "") {
	const auto result = NormalizeParam(Type(type), nullptr, input);
	if (result.ok || result.code != code || result.message.find(fragment) == std::string::npos) {
		std::cerr << id << ": " << type << " «" << input.text << "»: expected " << code << " «" << fragment << "», got "
		          << (result.ok ? "OK " + Describe(result.value) : result.code + " " + result.message) << "\n";
		failures++;
	}
}

ParamInput N(const char *json_number) {
	return ParamInput::Number(json_number);
}

ParamInput S(const std::string &text) {
	return ParamInput::String(text);
}

void TestInteger() {
	Accepts("integer number", "integer", N("204"), "204");
	Accepts("integer number -0", "integer", N("-0"), "0");
	Accepts("integer number 2^53", "integer", N("9007199254740992"), "9007199254740992");
	Accepts("integer number -2^53", "integer", N("-9007199254740992"), "-9007199254740992");
	Accepts("integer whole number", "integer", N("1.0e3"), "1000");
	Accepts("integer string", "integer", S("0012"), "12");
	Accepts("integer long string", "integer", S(std::string(38, '9')), std::string(38, '9'));
	Refuses("integer fraction", "integer", N("1.5"));
	Refuses("integer past 2^53", "integer", N("9007199254740994"));
	Refuses("integer string exponent", "integer", S("1e3"));
	Refuses("integer boolean", "integer", ParamInput::Boolean(true));
	Refuses("integer array", "integer", ParamInput::Array({N("1")}));
}

void TestNumber() {
	Accepts("number number", "number", N("0.1"), "0.1");
	Accepts("number exponent", "number", N("1e+21"), "1e+21");
	Accepts("number string", "number", S("-2.50"), "-2.5");
	Refuses("number string exponent", "number", S("1e5"));
	Refuses("number overflow", "number", N("1e400"));
	Refuses("number string NaN", "number", S("NaN"));
	Refuses("number boolean", "number", ParamInput::Boolean(false));
}

void TestDecimal() {
	Accepts("decimal number", "decimal(18,2)", N("1234.5"), "1234.5");
	Accepts("decimal number keeps digits", "decimal(18,2)", N("0.1"), "0.1");
	Accepts("decimal number exponent", "decimal(18,2)", N("1.25e2"), "125");
	Accepts("decimal number small exponent", "decimal(18,8)", N("1e-7"), "0.0000001");
	Accepts("decimal string", "decimal(18,2)", S("-0012.50"), "-12.5");
	Accepts("decimal string -0", "decimal(18,2)", S("-0.00"), "0");
	Refuses("decimal too many decimals", "decimal(18,2)", N("1.234"));
	Refuses("decimal too many digits", "decimal(5,2)", S("1234.5"));
	Refuses("decimal huge exponent", "decimal(18,2)", N("1e300"));
	Refuses("decimal boolean", "decimal(18,2)", ParamInput::Boolean(true));
}

void TestString() {
	Accepts("string string", "string", S("Иван"), "Иван");
	Accepts("string number", "string", N("204"), "204");
	Accepts("string number fraction", "string", N("0.10"), "0.1");
	Accepts("string boolean", "string", ParamInput::Boolean(true), "true");
	Accepts("string 4000 bytes", "string", S(std::string(4000, 'x')), std::string(4000, 'x'));
	Refuses("string 4001 bytes", "string", S(std::string(4001, 'x')), "E_PARAM_TOO_LONG");
	Refuses("string control", "string", S("a\x01"));
	Refuses("string NUL", "string", S(std::string("a\0", 2)));
	Refuses("string array", "string", ParamInput::Array({S("a")}));
}

void TestId() {
	Accepts("id string", "id", S("300000047114611"), "300000047114611");
	Accepts("id string zeros", "id", S("007"), "7");
	Accepts("id number", "id", N("300000047114611"), "300000047114611");
	Refuses("id negative number", "id", N("-1"));
	Refuses("id number past 2^53", "id", N("300000047114611000"));
	Refuses("id string sign", "id", S("-1"));
	Refuses("id fraction", "id", N("1.5"));
}

void TestDate() {
	Accepts("date string", "date", S("2026-10-29"), "2026-10-29");
	Refuses("date serial number", "date", N("46324"), "E_PARAM_TYPE", "строкой");
	Refuses("date invalid", "date", S("2026-02-30"));
	Refuses("date with time", "date", S("2026-10-29 10:00:00"));
}

void TestTimestamp() {
	Accepts("timestamp string", "timestamp", S("2026-10-29T08:30:00.500"), "2026-10-29 08:30:00.5");
	Refuses("timestamp serial number", "timestamp", N("46324.5"));
	Refuses("timestamp date only", "timestamp", S("2026-10-29"));
}

void TestBoolean() {
	Accepts("boolean true", "boolean", ParamInput::Boolean(true), "true");
	Accepts("boolean false", "boolean", ParamInput::Boolean(false), "false");
	Accepts("boolean string Y", "boolean", S("y"), "true");
	Accepts("boolean string 0", "boolean", S("0"), "false");
	Refuses("boolean number", "boolean", N("1"));
	Refuses("boolean string", "boolean", S("yes"));
}

void TestEnum() {
	Accepts("enum string", "enum(OPEN,PAID)", S("paid"), "PAID");
	Refuses("enum not listed", "enum(OPEN,PAID)", S("ALL"));
	Refuses("enum number", "enum(OPEN,PAID)", N("1"));
}

void TestList() {
	Accepts("list ids", "list<id>", ParamInput::Array({S("007"), N("8")}), "[7,8]");
	Accepts("list empty", "list<id>", ParamInput::Array({}), "[]");
	Accepts("list keeps order and duplicates", "list<integer>", ParamInput::Array({N("3"), N("1"), N("3")}), "[3,1,3]");
	Accepts("list<string> holds an empty string", "list<string>", ParamInput::Array({S(""), S("a")}), "[,a]");
	Refuses("list element", "list<id>", ParamInput::Array({S("1"), S("x")}), "E_PARAM_TYPE", "элемент 2");
	Refuses("list null element", "list<id>", ParamInput::Array({ParamInput::Null()}), "E_PARAM_TYPE", "элемент 1");
	Refuses("list nested", "list<id>", ParamInput::Array({ParamInput::Array({})}));
	Refuses("list scalar", "list<id>", S("1"), "E_PARAM_TYPE", "ожидается список");
	Refuses("list element too long", "list<string>", ParamInput::Array({S(std::string(4001, 'x'))}), "E_PARAM_TOO_LONG");
	std::vector<ParamInput> many(MAX_LIST_ELEMENTS + 1, N("1"));
	Refuses("list too many", "list<integer>", ParamInput::Array(many), "E_PARAM_TOO_LONG");
	std::vector<ParamInput> most(MAX_LIST_ELEMENTS, N("1"));
	const auto result = NormalizeParam(Type("list<integer>"), nullptr, ParamInput::Array(most));
	if (!result.ok || result.value.list.size() != MAX_LIST_ELEMENTS) {
		std::cerr << "list of 100000: refused\n";
		failures++;
	}
}

void TestEmptyValues() {
	const auto fallback = CanonicalValue::Scalar("204");
	for (const auto &empty : {ParamInput::Missing(), ParamInput::Null(), S("")}) {
		Accepts("empty takes the default", "integer", empty, "204", &fallback);
		Refuses("empty without a default", "integer", empty, "E_PARAM_MISSING");
	}
	const auto list_default = CanonicalValue::List({"1", "2"});
	Accepts("empty list input takes the default", "list<id>", ParamInput::Null(), "[1,2]", &list_default);
	// [] is a value, not an empty input.
	Accepts("[] is not empty", "list<id>", ParamInput::Array({}), "[]", &list_default);
	// A string parameter's "" is empty as well: the default, not "".
	const auto text_default = CanonicalValue::Scalar("x");
	Accepts("\"\" for a string takes the default", "string", S(""), "x", &text_default);
}

} // namespace

int main() {
	TestInteger();
	TestNumber();
	TestDecimal();
	TestString();
	TestId();
	TestDate();
	TestTimestamp();
	TestBoolean();
	TestEnum();
	TestList();
	TestEmptyValues();
	if (failures) {
		std::cerr << failures << " parameter value check(s) failed\n";
		return EXIT_FAILURE;
	}
	std::cout << "param_values_test: all checks passed\n";
	return EXIT_SUCCESS;
}
