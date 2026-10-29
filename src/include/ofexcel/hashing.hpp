#pragma once

// The hashes that identify a query definition and a dataset instance
// (CONTRACTS.md §1.1–1.3).
//
// Pure: OpenSSL and the standard library only. Every rule here has a test in
// test/cpp/hashing_test.cpp; a change to a rule changes every stored hash, so
// change the contract, the tests and a migration together.

#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace ofexcel {

//! The connection name an instance key uses for a query with `@engine: local`.
constexpr const char *LOCAL_CONNECTION_NAME = "local";

//! SHA-256 of `bytes`, 64 lower-case hex digits.
std::string Sha256Hex(const std::string &bytes);

//! `text` with a leading UTF-8 BOM removed and every "\r\n" and lone "\r"
//! turned into "\n". Nothing else changes (CONTRACTS.md §1.1).
std::string NormalizeDefinitionText(const std::string &text);

//! definition_hash: Sha256Hex(NormalizeDefinitionText(text)).
std::string DefinitionHash(const std::string &text);

//! One parameter's value, already in its type's canonical form (CONTRACTS.md
//! §3.1): a string, or for list<T> the elements in their original order.
struct CanonicalValue {
	bool is_list = false;
	std::string scalar;
	std::vector<std::string> list;

	static CanonicalValue Scalar(std::string value);
	static CanonicalValue List(std::vector<std::string> values);
};

//! canonical_params (CONTRACTS.md §1.2): a JSON object, keys sorted by their
//! UTF-8 bytes, no whitespace, strings escaped as RFC 8785 does — only `"`,
//! `\` and U+0000–U+001F, the latter as \b \t \n \f \r or \u00xx in lower-case
//! hex; everything else as its UTF-8 bytes. Throws std::invalid_argument if a
//! name or value is not valid UTF-8.
std::string CanonicalParams(const std::map<std::string, CanonicalValue> &params);

//! instance_key (CONTRACTS.md §1.3). `connection_name` is
//! LOCAL_CONNECTION_NAME for a local query.
std::string InstanceKey(const std::string &query_id, const std::string &definition_hash,
                        const std::string &connection_name, const std::string &canonical_params);

} // namespace ofexcel
