// Result column types (CONTRACTS.md §3.2, §3.3): T1–T26 for Oracle columns,
// J1–J8 for the JSON the add-in gets, by their ids. Pure: no DuckDB.

#include "ofexcel/result_types.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

using namespace ofexcel;

namespace {

int failures = 0;

ColumnType Column(const std::string &text) {
	ColumnType type;
	std::string error;
	if (!ParseColumnType(text, type, error)) {
		std::cerr << "cannot parse column type " << text << ": " << error << "\n";
		failures++;
	}
	return type;
}

ColumnInference Infer(const std::string &name, const std::vector<std::string> &values, const char *declared = nullptr,
                      const char *start = nullptr) {
	ColumnType declared_type;
	ColumnType start_type;
	if (declared) {
		declared_type = Column(declared);
	}
	if (start) {
		start_type = Column(start);
	}
	OracleColumnInference inference(name, declared ? &declared_type : nullptr, start ? &start_type : nullptr);
	for (const auto &value : values) {
		inference.Add(value);
	}
	return inference.Finish();
}

//! "∅" for NULL, the stored text otherwise, "MISMATCH" for a value the type refuses.
std::vector<std::string> Stored(const ColumnType &type, const std::vector<std::string> &values) {
	std::vector<std::string> stored;
	for (const auto &value : values) {
		std::string text;
		switch (ConvertOracleValue(type, value, text)) {
		case StoredValue::VALUE:
			stored.push_back(text);
			break;
		case StoredValue::NULL_VALUE:
			stored.push_back("∅");
			break;
		case StoredValue::MISMATCH:
			stored.push_back("MISMATCH");
			break;
		}
	}
	return stored;
}

void Check(const char *id, bool condition, const std::string &detail) {
	if (!condition) {
		std::cerr << id << ": " << detail << "\n";
		failures++;
	}
}

std::string Join(const std::vector<std::string> &values) {
	std::string joined;
	for (const auto &value : values) {
		joined += (joined.empty() ? "" : ", ") + value;
	}
	return joined;
}

//! The inferred type is `expected`, with or without warnings as `warned` says,
//! and the stored values are `stored` (when given).
void ExpectType(const char *id, const ColumnInference &inference, const std::string &expected, bool warned,
                const std::vector<std::string> &values = {}, const std::vector<std::string> &stored = {}) {
	Check(id, inference.ok, "unexpected error: " + inference.message);
	Check(id, inference.type.ToString() == expected, "type " + inference.type.ToString() + ", expected " + expected);
	Check(id, inference.warnings.empty() != warned,
	      std::string(warned ? "expected a warning" : "unexpected warning: ") + Join(inference.warnings));
	if (!stored.empty()) {
		const auto actual = Stored(inference.type, values);
		Check(id, actual == stored, "stored " + Join(actual) + ", expected " + Join(stored));
	}
}

void TestOracleVectors() {
	ExpectType("T1", Infer("AMOUNT", {"1", "2", "300"}), "integer", false);
	ExpectType("T2", Infer("VENDOR_ID", {"1", "2", "300"}), "id", false);
	ExpectType("T3", Infer("REF", {"1", "1234567890123456"}), "id", false);
	ExpectType("T4", Infer("CODE", {"0012", "13"}), "string", false);
	ExpectType("T5", Infer("AMT", {".84", "-1.5", "3"}), "decimal(38,2)", false, {".84", "-1.5", "3"},
	           {"0.84", "-1.5", "3"});
	ExpectType("T6", Infer("X", {"1.5", "abc"}), "string", false);
	ExpectType("T7", Infer("D", {"2026-01-01", "2026-02-01"}), "date", false);
	ExpectType("T8", Infer("D", {"2026-01-01 00:00:00", "2026-02-01 00:00:00"}), "date", false,
	           {"2026-01-01 00:00:00", "2026-02-01 00:00:00"}, {"2026-01-01", "2026-02-01"});
	ExpectType("T9", Infer("D", {"2026-01-01 10:00:00"}), "timestamp", false);
	ExpectType("T10", Infer("N", {"", ""}), "string", false);
	ExpectType("T11", Infer("E", {"1e5"}), "string", false);
	{
		const auto inference = Infer("AMOUNT", {"1.5"}, nullptr, "integer");
		ExpectType("T12", inference, "decimal(38,1)", true);
		Check("T12", inference.widened, "not marked as widened");
	}
	{
		const auto inference = Infer("D", {"n/a"}, nullptr, "date");
		ExpectType("T13", inference, "string", true);
		Check("T13", inference.widened, "not marked as widened");
	}
	{
		const auto inference = Infer("AMOUNT", {"7"}, nullptr, "decimal(38,2)");
		ExpectType("T14", inference, "decimal(38,2)", false);
		Check("T14", !inference.widened, "marked as widened");
	}
	{
		const auto inference = Infer("VENDOR_ID", {"12", "x"}, "integer");
		Check("T15", !inference.ok && inference.code == "E_TYPE_DECLARED_MISMATCH" && inference.row == 2 &&
		                 inference.value == "x" && inference.message.find("VENDOR_ID") != std::string::npos &&
		                 inference.message.find("строке 2") != std::string::npos,
		      "expected E_TYPE_DECLARED_MISMATCH on row 2 for x, got " + inference.code + " " + inference.message);
	}
	ExpectType("T16", Infer("AMOUNT", {"", "5", ""}), "integer", false, {"", "5", ""}, {"∅", "5", "∅"});
	ExpectType("T17", Infer("N", {"", ""}), "string", false, {"", ""}, {"∅", "∅"});
	ExpectType("T18", Infer("TS", {"2026-01-01 10:11:12.123456789"}), "timestamp", false,
	           {"2026-01-01 10:11:12.123456789", "2026-01-01 10:11:12.999999999"},
	           {"2026-01-01 10:11:12.123456", "2026-01-01 10:11:12.999999"});
	ExpectType("T19", Infer("CREATION_DATE", {"2024-09-03 13:30:19.001000", "2024-10-01 11:11:46.171000"}), "timestamp",
	           false, {"2024-09-03 13:30:19.001000"}, {"2024-09-03 13:30:19.001"});
	ExpectType("T20", Infer("TZ", {"2026-10-29 14:04:12.418151 +00:00"}), "timestamptz", false,
	           {"2026-10-29 14:04:12.418151 +00:00"}, {"2026-10-29 14:04:12.418151"});
	ExpectType("T21", Infer("TZ", {"2026-10-29 17:04:12 +03:00"}), "timestamptz", false, {"2026-10-29 17:04:12 +03:00"},
	           {"2026-10-29 14:04:12"});
	ExpectType("T22", Infer("TZ", {"2026-10-29 14:04:12 +00:00", "2026-10-29 14:04:12"}), "string", false);
	ExpectType("T23", Infer("D", {"2026-10-29"}), "date", false, {"2026-10-29"}, {"2026-10-29"});
	ExpectType("T24", Infer("X", {"1.5", "Inf"}), "string", false);
	ExpectType("T25", Infer("TS", {"2026-01-01 00:00:00.000000000", "2026-01-02 00:00:00"}), "date", false,
	           {"2026-01-01 00:00:00.000000000"}, {"2026-01-01"});
	ExpectType("T26", Infer("D", {"2026-10-29 14:05:52.000000"}), "timestamp", false, {"2026-10-29 14:05:52.000000"},
	           {"2026-10-29 14:05:52"});
}

void TestInferenceEdges() {
	ExpectType("_ID in any case", Infer("vendor_Id", {"7"}), "id", false);
	ExpectType("15 digits is integer", Infer("N", {"123456789012345"}), "integer", false);
	ExpectType("negative integer", Infer("N", {"-5", "0"}), "integer", false);
	ExpectType("-.5 is decimal", Infer("N", {"-.5"}), "decimal(38,1)", false, {"-.5"}, {"-0.5"});
	ExpectType("decimal at 38 digits", Infer("N", {std::string(36, '9') + ".25"}), "decimal(38,2)", false);
	ExpectType("decimal past 38 digits", Infer("N", {std::string(37, '9') + ".25"}), "number", true);
	ExpectType("dates and timestamps mixed", Infer("D", {"2026-01-01", "2026-01-01 10:00:00"}), "string", false);
	ExpectType("invalid date", Infer("D", {"2026-02-30"}), "string", false);
	ExpectType("TSTZ without space", Infer("TZ", {"2026-10-29 14:04:12-05:00"}), "timestamptz", false,
	           {"2026-10-29 22:04:12-05:00"}, {"2026-10-30 03:04:12"});
	ExpectType("TSTZ over midnight back", Infer("TZ", {"2026-01-01 01:00:00 +03:00"}), "timestamptz", false,
	           {"2026-01-01 01:00:00 +03:00", "2024-03-01 00:30:00 +01:00"},
	           {"2025-12-31 22:00:00", "2024-02-29 23:30:00"});
	ExpectType("boolean is never inferred", Infer("F", {"Y", "N"}), "string", false);
	ExpectType("all NULL keeps the start type", Infer("N", {"", ""}, nullptr, "integer"), "integer", false);
	ExpectType("integer widens past 15 digits", Infer("N", {"1234567890123456"}, nullptr, "integer"), "decimal(38,0)",
	           true);
	ExpectType("decimal widens its scale", Infer("N", {"1.125"}, nullptr, "decimal(38,2)"), "decimal(38,3)", true);
	ExpectType("integer widens to number", Infer("N", {std::string(30, '9') + "." + std::string(10, '9')}, nullptr,
	                                               "integer"),
	           "number", true);
	ExpectType("date widens to timestamp", Infer("D", {"2026-01-01 10:00:00"}, nullptr, "date"), "timestamp", true);
	ExpectType("id widens to string", Infer("VENDOR_ID", {"A1"}, nullptr, "id"), "string", true);
	ExpectType("timestamp holds dates", Infer("D", {"2026-01-01"}, nullptr, "timestamp"), "timestamp", false,
	           {"2026-01-01"}, {"2026-01-01 00:00:00"});
	ExpectType("chains do not cross", Infer("D", {"7"}, nullptr, "date"), "string", true);
	ExpectType("declared wins over start", Infer("N", {"x"}, "string", "integer"), "string", false);
	ExpectType("declared number takes Inf", Infer("X", {"1.5", "Inf", "1e5"}, "number"), "number", false,
	           {"1.5", "Inf", "-Inf", "1e5", "NaN"}, {"1.5", "inf", "-inf", "1e+05", "nan"});
	ExpectType("declared boolean", Infer("F", {"Y", "n", "TRUE", "0"}, "boolean"), "boolean", false, {"Y", "n", "0"},
	           {"true", "false", "false"});
	{
		const auto inference = Infer("D", {"2026-01-01", "", "2026-01-01 10:00:00"}, "date");
		Check("declared date refuses a time", !inference.ok && inference.row == 3, inference.message);
	}
	{
		const auto inference = Infer("A", {"1.234"}, "decimal(18,2)");
		Check("declared decimal refuses a lost digit", !inference.ok && inference.row == 1, inference.message);
	}
	Check("hex is not a number", Stored(Column("number"), {"0x1e5"}) == std::vector<std::string> {"MISMATCH"},
	      Join(Stored(Column("number"), {"0x1e5"})));
}

void ExpectCell(const char *id, const char *type, const char *text, JsonCell::Kind kind, const std::string &expected,
                bool non_finite = false) {
	const std::string value = text ? text : "";
	const auto cell = EncodeJsonValue(Column(type), text ? &value : nullptr);
	std::string actual;
	switch (cell.kind) {
	case JsonCell::Kind::NULL_VALUE:
		actual = "null";
		break;
	case JsonCell::Kind::STRING:
		actual = cell.text;
		break;
	case JsonCell::Kind::INTEGER:
		actual = std::to_string(cell.integer);
		break;
	case JsonCell::Kind::NUMBER:
		actual = std::to_string(cell.number);
		break;
	case JsonCell::Kind::BOOLEAN:
		actual = cell.boolean ? "true" : "false";
		break;
	}
	Check(id, cell.kind == kind && actual == expected && cell.non_finite == non_finite,
	      "got «" + actual + "» of kind " + std::to_string(static_cast<int>(cell.kind)) + ", expected «" + expected + "»");
}

void TestJsonVectors() {
	using Kind = JsonCell::Kind;
	ExpectCell("J1", "decimal(18,2)", "1234567890123456.78", Kind::STRING, "1234567890123456.78");
	ExpectCell("J2", "decimal(18,2)", "-0.50", Kind::STRING, "-0.5");
	ExpectCell("J3", "id", "300000012345678901", Kind::STRING, "300000012345678901");
	ExpectCell("J4", "integer", "42", Kind::INTEGER, "42");
	ExpectCell("J5", "date", "2026-03-31", Kind::STRING, "2026-03-31");
	ExpectCell("J6", "timestamp", "2026-03-31 23:59:59.120000", Kind::STRING, "2026-03-31 23:59:59.12");
	ExpectCell("J7", "number", "nan", Kind::NULL_VALUE, "null", true);
	for (const auto *type : {"id", "integer", "decimal(18,2)", "number", "string", "text", "date", "timestamp",
	                         "timestamptz", "boolean"}) {
		ExpectCell("J8", type, nullptr, Kind::NULL_VALUE, "null");
	}
	ExpectCell("J7 inf", "number", "inf", Kind::NULL_VALUE, "null", true);
	ExpectCell("number", "number", "1.5", Kind::NUMBER, std::to_string(1.5));
	ExpectCell("decimal zero", "decimal(18,2)", "0.00", Kind::STRING, "0");
	ExpectCell("timestamptz", "timestamptz", "2026-10-29 14:04:12.418151", Kind::STRING, "2026-10-29T14:04:12.418151Z");
	ExpectCell("timestamptz whole second", "timestamptz", "2026-10-29 14:04:12", Kind::STRING, "2026-10-29T14:04:12Z");
	ExpectCell("timestamp nanoseconds cut", "timestamp", "2026-03-31 23:59:59.123456789", Kind::STRING,
	           "2026-03-31 23:59:59.123456");
	ExpectCell("boolean", "boolean", "true", Kind::BOOLEAN, "true");
	ExpectCell("boolean false", "boolean", "false", Kind::BOOLEAN, "false");
	ExpectCell("string as is", "string", " a\"b ", Kind::STRING, " a\"b ");
}

} // namespace

int main() {
	TestOracleVectors();
	TestInferenceEdges();
	TestJsonVectors();
	if (failures) {
		std::cerr << failures << " result type check(s) failed\n";
		return EXIT_FAILURE;
	}
	std::cout << "result_types_test: all checks passed\n";
	return EXIT_SUCCESS;
}
