#include "ofexcel/json_body.hpp"

#include "yyjson.hpp"

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <stdexcept>

using namespace duckdb_yyjson; // NOLINT: yyjson's C-style API is unusable qualified

namespace ofexcel {

struct JsonObject::Impl {
	yyjson_mut_doc *doc;
	yyjson_mut_val *root;

	Impl() : doc(yyjson_mut_doc_new(nullptr)) {
		if (!doc) {
			throw std::bad_alloc();
		}
		root = yyjson_mut_obj(doc);
		yyjson_mut_doc_set_root(doc, root);
	}
	~Impl() {
		yyjson_mut_doc_free(doc);
	}

	//! Keys are copied into the document: yyjson only stores a pointer otherwise.
	void Add(const std::string &key, yyjson_mut_val *value) {
		yyjson_mut_obj_add(root, yyjson_mut_strncpy(doc, key.data(), key.size()), value);
	}
};

JsonObject::JsonObject() : impl(std::make_unique<Impl>()) {
}
JsonObject::~JsonObject() = default;
JsonObject::JsonObject(JsonObject &&) noexcept = default;
JsonObject &JsonObject::operator=(JsonObject &&) noexcept = default;

JsonObject &JsonObject::Set(const std::string &key, const std::string &value) {
	impl->Add(key, yyjson_mut_strncpy(impl->doc, value.data(), value.size()));
	return *this;
}

JsonObject &JsonObject::Set(const std::string &key, const char *value) {
	return Set(key, std::string(value));
}

JsonObject &JsonObject::Set(const std::string &key, int64_t value) {
	impl->Add(key, yyjson_mut_sint(impl->doc, value));
	return *this;
}

JsonObject &JsonObject::Set(const std::string &key, bool value) {
	impl->Add(key, yyjson_mut_bool(impl->doc, value));
	return *this;
}

JsonObject &JsonObject::SetNull(const std::string &key) {
	impl->Add(key, yyjson_mut_null(impl->doc));
	return *this;
}

JsonObject &JsonObject::Set(const std::string &key, const JsonObject &value) {
	impl->Add(key, yyjson_mut_val_mut_copy(impl->doc, value.impl->root));
	return *this;
}

std::string JsonObject::Serialize() const {
	size_t length = 0;
	char *text = yyjson_mut_write(impl->doc, YYJSON_WRITE_NOFLAG, &length);
	if (!text) {
		throw std::runtime_error("yyjson_mut_write failed");
	}
	std::string serialized(text, length);
	free(text);
	return serialized;
}

std::string ErrorBody(const std::string &code, const std::string &message) {
	JsonObject error;
	error.Set("code", code).Set("message", message);
	JsonObject body;
	body.Set("error", error);
	return body.Serialize();
}

std::string UtcNowIso8601() {
	const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
	std::tm utc {};
#ifdef _WIN32
	gmtime_s(&utc, &now);
#else
	gmtime_r(&now, &utc);
#endif
	char buffer[sizeof "2026-01-01T00:00:00Z"];
	std::strftime(buffer, sizeof buffer, "%Y-%m-%dT%H:%M:%SZ", &utc);
	return buffer;
}

} // namespace ofexcel
