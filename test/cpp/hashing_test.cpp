// definition_hash, canonical_params and instance_key (CONTRACTS.md §1.1–1.3).
// Pure: OpenSSL only, no DuckDB. The fixed SHA-256 values were computed with
// `shasum -a 256` over the same bytes, not with this code.

#include "ofexcel/hashing.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

using namespace ofexcel;

namespace {

int failures = 0;

void Expect(const char *id, const std::string &actual, const std::string &expected) {
	if (actual != expected) {
		std::cerr << id << ": got \"" << actual << "\", expected \"" << expected << "\"\n";
		failures++;
	}
}

void ExpectTrue(const char *id, bool condition) {
	if (!condition) {
		std::cerr << id << ": failed\n";
		failures++;
	}
}

void ExpectInvalid(const char *id, const std::map<std::string, CanonicalValue> &params) {
	try {
		CanonicalParams(params);
		std::cerr << id << ": accepted invalid UTF-8\n";
		failures++;
	} catch (const std::invalid_argument &) {
	}
}

const std::string QUERY_TEXT = "-- @query: ap_invoices\n"
                               "-- Счета AP\n"
                               "SELECT invoice_id FROM ap_invoices_all WHERE org_id = @org_id\n";

void TestKnownDefinitionHashes() {
	Expect("H1 empty", DefinitionHash(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
	Expect("H2 one line", DefinitionHash("SELECT 1 FROM dual\n"),
	       "2daa1e1f1ac199f7626211b8198f73387380392959718138a5d7b70ad5737695");
	Expect("H3 query file", DefinitionHash(QUERY_TEXT),
	       "dace7cc3b17597f09fba45edecd2aa0a614bd353dddb3b40356c08d48c05ee71");
}

void TestLineEndingsAndBomDoNotMatter() {
	const auto lf = DefinitionHash(QUERY_TEXT);
	std::string crlf;
	std::string cr;
	for (const char c : QUERY_TEXT) {
		crlf += c == '\n' ? std::string("\r\n") : std::string(1, c);
		cr += c == '\n' ? '\r' : c;
	}
	Expect("H4 CRLF", DefinitionHash(crlf), lf);
	Expect("H5 CR", DefinitionHash(cr), lf);
	Expect("H6 BOM", DefinitionHash("\xEF\xBB\xBF" + QUERY_TEXT), lf);
	Expect("H7 BOM and CRLF", DefinitionHash("\xEF\xBB\xBF" + crlf), lf);
	// "\r\r\n" is a lone CR followed by a CRLF: two line ends.
	Expect("H8 CR CRLF", NormalizeDefinitionText("a\r\r\nb"), "a\n\nb");
	Expect("H9 trailing CR", NormalizeDefinitionText("a\r"), "a\n");
}

void TestNothingElseIsNormalized() {
	const auto lf = DefinitionHash(QUERY_TEXT);
	auto spaced = QUERY_TEXT;
	spaced.replace(spaced.find("WHERE"), 5, "WHERE ");
	ExpectTrue("H10 one more space", DefinitionHash(spaced) != lf);
	ExpectTrue("H11 trailing space", DefinitionHash(QUERY_TEXT + " ") != lf);
	ExpectTrue("H12 case", DefinitionHash("select 1 from dual\n") != DefinitionHash("SELECT 1 FROM dual\n"));
	ExpectTrue("H13 tab vs space", DefinitionHash("a\tb") != DefinitionHash("a b"));
	ExpectTrue("H14 final newline", DefinitionHash("SELECT 1 FROM dual") != DefinitionHash("SELECT 1 FROM dual\n"));
	// A BOM anywhere but at the start is content.
	ExpectTrue("H15 BOM in the middle", DefinitionHash("a\xEF\xBB\xBF" "b") != DefinitionHash("ab"));
	Expect("H16 only one BOM stripped", NormalizeDefinitionText("\xEF\xBB\xBF\xEF\xBB\xBF" "a"), "\xEF\xBB\xBF" "a");
}

void TestCanonicalParams() {
	Expect("P1 empty", CanonicalParams({}), "{}");

	std::map<std::string, CanonicalValue> params;
	params.emplace("org_id", CanonicalValue::Scalar("204"));
	params.emplace("ids", CanonicalValue::List({"3", "1"}));
	Expect("P2 sorted, list in original order", CanonicalParams(params), R"({"ids":["3","1"],"org_id":"204"})");

	// Insertion order cannot matter: the same entries, inserted the other way round.
	std::map<std::string, CanonicalValue> reversed;
	reversed.emplace("ids", CanonicalValue::List({"3", "1"}));
	reversed.emplace("org_id", CanonicalValue::Scalar("204"));
	Expect("P3 insertion order", CanonicalParams(reversed), CanonicalParams(params));

	// Byte order: upper case before lower case, "_" (0x5F) between them,
	// a prefix before its extensions, Cyrillic (0xD0…) after ASCII.
	std::map<std::string, CanonicalValue> names;
	for (const auto *name : {"b", "a_b", "ab", "B", "a", "\xD0\xB0" /* Cyrillic a */, "a_"}) {
		names.emplace(name, CanonicalValue::Scalar("1"));
	}
	Expect("P4 key order", CanonicalParams(names),
	       "{\"B\":\"1\",\"a\":\"1\",\"a_\":\"1\",\"a_b\":\"1\",\"ab\":\"1\",\"b\":\"1\",\"\xD0\xB0\":\"1\"}");

	std::map<std::string, CanonicalValue> escaped;
	escaped.emplace("s", CanonicalValue::Scalar("Zoë \"Z\" \\ / \t\n\r\b\f\x01\x1f\x7f"));
	Expect("P5 escaping", CanonicalParams(escaped),
	       "{\"s\":\"Zoë \\\"Z\\\" \\\\ / \\t\\n\\r\\b\\f\\u0001\\u001f\x7f\"}");

	std::map<std::string, CanonicalValue> lists;
	lists.emplace("empty", CanonicalValue::List({}));
	lists.emplace("blank", CanonicalValue::Scalar(""));
	Expect("P6 empty list and empty string", CanonicalParams(lists), R"({"blank":"","empty":[]})");

	ExpectInvalid("P7 lone continuation byte", {{"s", CanonicalValue::Scalar("\x80")}});
	ExpectInvalid("P8 overlong", {{"s", CanonicalValue::Scalar("\xC0\xAF")}});
	ExpectInvalid("P9 surrogate", {{"s", CanonicalValue::Scalar("\xED\xA0\x80")}});
	ExpectInvalid("P10 truncated", {{"s", CanonicalValue::Scalar("\xD0")}});
	ExpectInvalid("P11 above U+10FFFF", {{"s", CanonicalValue::Scalar("\xF4\x90\x80\x80")}});
	ExpectInvalid("P12 invalid name", {{"\xFF", CanonicalValue::Scalar("1")}});
	ExpectInvalid("P13 invalid list element", {{"s", CanonicalValue::List({"ok", "\xC3"})}});
	// Four-byte characters are fine.
	Expect("P14 emoji", CanonicalParams({{"s", CanonicalValue::Scalar("\xF0\x9F\x98\x80")}}),
	       "{\"s\":\"\xF0\x9F\x98\x80\"}");
}

void TestInstanceKey() {
	const auto hash = DefinitionHash(QUERY_TEXT);
	std::map<std::string, CanonicalValue> params;
	params.emplace("org_id", CanonicalValue::Scalar("204"));
	params.emplace("ids", CanonicalValue::List({"3", "1"}));
	const auto key = InstanceKey("ap_invoices", hash, "fusion_prod", CanonicalParams(params));
	Expect("K1 known value", key, "7b37150da72bcd783fb28ca9ef8dacfb2cb9e6ac8958983738e378844e73a077");
	Expect("K2 local query", InstanceKey("local_summary", hash, LOCAL_CONNECTION_NAME, CanonicalParams({})),
	       "ba560e8e5599d00f1e6d7df7efbda760a09d56e7c490dd91ca8ffa5fb5cc7d00");

	// Each component moves the key.
	ExpectTrue("K3 connection", InstanceKey("ap_invoices", hash, "fusion_test", CanonicalParams(params)) != key);
	ExpectTrue("K4 query id", InstanceKey("ap_invoices2", hash, "fusion_prod", CanonicalParams(params)) != key);
	ExpectTrue("K5 definition",
	           InstanceKey("ap_invoices", DefinitionHash(QUERY_TEXT + " "), "fusion_prod", CanonicalParams(params)) != key);
	params["ids"] = CanonicalValue::List({"1", "3"});
	ExpectTrue("K6 list order", InstanceKey("ap_invoices", hash, "fusion_prod", CanonicalParams(params)) != key);
}

void TestCanonicalJson() {
	// Keys sorted by bytes at every level, whatever order they were set in.
	const auto nested = CanonicalJson::Object()
	                        .Set("b", CanonicalJson::Integer(-2))
	                        .Set("a", CanonicalJson::Array()
	                                      .Add(CanonicalJson::Null())
	                                      .Add(CanonicalJson::Boolean(true))
	                                      .Add(CanonicalJson::Object().Set("z", CanonicalJson::String("1")).Set("Z", CanonicalJson::String("2"))))
	                        .Set("\xC3\xA9", CanonicalJson::String("\xC3\xA9"))
	                        .Set("A", CanonicalJson::Boolean(false));
	Expect("CJ1 nesting and order", nested.Serialize(),
	       "{\"A\":false,\"a\":[null,true,{\"Z\":\"2\",\"z\":\"1\"}],\"b\":-2,\"\xC3\xA9\":\"\xC3\xA9\"}");
	// The same escaping as canonical_params: lower-case hex, / and DEL as they are.
	Expect("CJ2 escaping", CanonicalJson::String(std::string("\x01\"\\\n/\x7F", 6)).Serialize(),
	       "\"\\u0001\\\"\\\\\\n/\x7F\"");
	Expect("CJ3 a key set twice keeps the last value",
	       CanonicalJson::Object().Set("k", CanonicalJson::Integer(1)).Set("k", CanonicalJson::Integer(2)).Serialize(),
	       "{\"k\":2}");
	Expect("CJ4 empty containers", CanonicalJson::Object().Set("x", CanonicalJson::Array()).Serialize(), "{\"x\":[]}");
	bool refused = false;
	try {
		CanonicalJson::String("\xFF").Serialize();
	} catch (const std::invalid_argument &) {
		refused = true;
	}
	ExpectTrue("CJ5 invalid UTF-8", refused);
}

} // namespace

int main() {
	TestKnownDefinitionHashes();
	TestLineEndingsAndBomDoNotMatter();
	TestNothingElseIsNormalized();
	TestCanonicalParams();
	TestInstanceKey();
	TestCanonicalJson();
	if (failures) {
		std::cerr << failures << " hashing check(s) failed\n";
		return EXIT_FAILURE;
	}
	std::cout << "hashing_test: all checks passed\n";
	return EXIT_SUCCESS;
}
