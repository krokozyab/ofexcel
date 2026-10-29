#include "ofexcel/hashing.hpp"

#include "ofexcel/utf8.hpp"

#include <openssl/evp.h>

#include <algorithm>
#include <cstring>

namespace ofexcel {

namespace {

void AppendJsonString(std::string &out, const std::string &value) {
	if (!IsValidUtf8(value)) {
		throw std::invalid_argument("canonical_params: not valid UTF-8");
	}
	static const char *const HEX = "0123456789abcdef";
	out += '"';
	for (const char c : value) {
		const auto byte = static_cast<unsigned char>(c);
		switch (byte) {
		case '"':
			out += "\\\"";
			break;
		case '\\':
			out += "\\\\";
			break;
		case '\b':
			out += "\\b";
			break;
		case '\t':
			out += "\\t";
			break;
		case '\n':
			out += "\\n";
			break;
		case '\f':
			out += "\\f";
			break;
		case '\r':
			out += "\\r";
			break;
		default:
			if (byte < 0x20) {
				out += "\\u00";
				out += HEX[byte >> 4];
				out += HEX[byte & 0x0f];
			} else {
				out += c;
			}
		}
	}
	out += '"';
}

//! Byte order, whatever the signedness of char.
bool ByteLess(const std::string &left, const std::string &right) {
	const size_t common = std::min(left.size(), right.size());
	const int order = std::memcmp(left.data(), right.data(), common);
	return order != 0 ? order < 0 : left.size() < right.size();
}

} // namespace

CanonicalValue CanonicalValue::Scalar(std::string value) {
	CanonicalValue result;
	result.scalar = std::move(value);
	return result;
}

CanonicalValue CanonicalValue::List(std::vector<std::string> values) {
	CanonicalValue result;
	result.is_list = true;
	result.list = std::move(values);
	return result;
}

std::string Sha256Hex(const std::string &bytes) {
	unsigned char digest[EVP_MAX_MD_SIZE];
	unsigned int size = 0;
	if (EVP_Digest(bytes.data(), bytes.size(), digest, &size, EVP_sha256(), nullptr) != 1) {
		throw std::runtime_error("SHA-256 failed");
	}
	static const char *const HEX = "0123456789abcdef";
	std::string hex;
	hex.reserve(size * 2);
	for (unsigned int i = 0; i < size; i++) {
		hex += HEX[digest[i] >> 4];
		hex += HEX[digest[i] & 0x0f];
	}
	return hex;
}

std::string NormalizeDefinitionText(const std::string &text) {
	size_t start = 0;
	if (text.size() >= 3 && text.compare(0, 3, "\xEF\xBB\xBF") == 0) {
		start = 3;
	}
	std::string normalized;
	normalized.reserve(text.size() - start);
	for (size_t i = start; i < text.size(); i++) {
		if (text[i] != '\r') {
			normalized += text[i];
			continue;
		}
		normalized += '\n';
		if (i + 1 < text.size() && text[i + 1] == '\n') {
			i++;
		}
	}
	return normalized;
}

std::string DefinitionHash(const std::string &text) {
	return Sha256Hex(NormalizeDefinitionText(text));
}

std::string CanonicalParams(const std::map<std::string, CanonicalValue> &params) {
	std::vector<const std::pair<const std::string, CanonicalValue> *> entries;
	for (const auto &entry : params) {
		entries.push_back(&entry);
	}
	std::sort(entries.begin(), entries.end(),
	          [](const auto *left, const auto *right) { return ByteLess(left->first, right->first); });

	std::string json = "{";
	for (size_t i = 0; i < entries.size(); i++) {
		if (i) {
			json += ',';
		}
		AppendJsonString(json, entries[i]->first);
		json += ':';
		const auto &value = entries[i]->second;
		if (!value.is_list) {
			AppendJsonString(json, value.scalar);
			continue;
		}
		json += '[';
		for (size_t k = 0; k < value.list.size(); k++) {
			if (k) {
				json += ',';
			}
			AppendJsonString(json, value.list[k]);
		}
		json += ']';
	}
	return json + "}";
}

std::string InstanceKey(const std::string &query_id, const std::string &definition_hash,
                        const std::string &connection_name, const std::string &canonical_params) {
	return Sha256Hex(query_id + "\n" + definition_hash + "\n" + connection_name + "\n" + canonical_params);
}

} // namespace ofexcel
