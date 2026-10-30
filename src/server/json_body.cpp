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

JsonArrayRef JsonObject::SetArray(const std::string &key) {
	auto *array = yyjson_mut_arr(impl->doc);
	impl->Add(key, array);
	return JsonArrayRef(impl->doc, array);
}

namespace {

yyjson_mut_doc *DocOf(void *doc) {
	return static_cast<yyjson_mut_doc *>(doc);
}

yyjson_mut_val *ArrayOf(void *array) {
	return static_cast<yyjson_mut_val *>(array);
}

} // namespace

JsonArrayRef &JsonArrayRef::AddString(const std::string &value) {
	yyjson_mut_arr_append(ArrayOf(array), yyjson_mut_strncpy(DocOf(doc), value.data(), value.size()));
	return *this;
}

JsonArrayRef &JsonArrayRef::AddInt(int64_t value) {
	yyjson_mut_arr_append(ArrayOf(array), yyjson_mut_sint(DocOf(doc), value));
	return *this;
}

JsonArrayRef &JsonArrayRef::AddDouble(double value) {
	yyjson_mut_arr_append(ArrayOf(array), yyjson_mut_real(DocOf(doc), value));
	return *this;
}

JsonArrayRef &JsonArrayRef::AddBool(bool value) {
	yyjson_mut_arr_append(ArrayOf(array), yyjson_mut_bool(DocOf(doc), value));
	return *this;
}

JsonArrayRef &JsonArrayRef::AddNull() {
	yyjson_mut_arr_append(ArrayOf(array), yyjson_mut_null(DocOf(doc)));
	return *this;
}

JsonArrayRef &JsonArrayRef::AddObject(const JsonObject &value) {
	yyjson_mut_arr_append(ArrayOf(array), yyjson_mut_val_mut_copy(DocOf(doc), value.impl->root));
	return *this;
}

JsonArrayRef JsonArrayRef::AddArray() {
	auto *nested = yyjson_mut_arr(DocOf(doc));
	yyjson_mut_arr_append(ArrayOf(array), nested);
	return JsonArrayRef(doc, nested);
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

namespace {

yyjson_val *ValueOf(void *value) {
	return static_cast<yyjson_val *>(value);
}

} // namespace

JsonKind JsonValue::Kind() const {
	switch (yyjson_get_type(ValueOf(value))) {
	case YYJSON_TYPE_NULL:
		return JsonKind::NULL_VALUE;
	case YYJSON_TYPE_BOOL:
		return JsonKind::BOOLEAN;
	case YYJSON_TYPE_NUM:
	case YYJSON_TYPE_RAW:
		// Numbers are read as their raw text (JsonDocument::Parse).
		return JsonKind::NUMBER;
	case YYJSON_TYPE_STR:
		return JsonKind::STRING;
	case YYJSON_TYPE_ARR:
		return JsonKind::ARRAY;
	case YYJSON_TYPE_OBJ:
		return JsonKind::OBJECT;
	default:
		// yyjson reports a null pointer as YYJSON_TYPE_NONE.
		return JsonKind::MISSING;
	}
}

std::string JsonValue::String() const {
	if (!yyjson_is_str(ValueOf(value))) {
		return "";
	}
	return std::string(yyjson_get_str(ValueOf(value)), yyjson_get_len(ValueOf(value)));
}

bool JsonValue::Bool() const {
	return yyjson_is_bool(ValueOf(value)) && yyjson_get_bool(ValueOf(value));
}

std::string JsonValue::NumberText() const {
	const auto *raw = yyjson_get_raw(ValueOf(value));
	return yyjson_is_raw(ValueOf(value)) && raw ? std::string(raw, yyjson_get_len(ValueOf(value))) : "";
}

std::vector<std::string> JsonValue::Keys() const {
	std::vector<std::string> keys;
	if (!yyjson_is_obj(ValueOf(value))) {
		return keys;
	}
	yyjson_obj_iter iterator = yyjson_obj_iter_with(ValueOf(value));
	while (auto *key = yyjson_obj_iter_next(&iterator)) {
		keys.emplace_back(yyjson_get_str(key), yyjson_get_len(key));
	}
	return keys;
}

JsonValue JsonValue::Get(const std::string &key) const {
	if (!yyjson_is_obj(ValueOf(value))) {
		return JsonValue(nullptr);
	}
	return JsonValue(yyjson_obj_getn(ValueOf(value), key.data(), key.size()));
}

size_t JsonValue::Size() const {
	return yyjson_is_arr(ValueOf(value)) ? yyjson_arr_size(ValueOf(value)) : 0;
}

JsonValue JsonValue::At(size_t index) const {
	if (index >= Size()) {
		return JsonValue(nullptr);
	}
	return JsonValue(yyjson_arr_get(ValueOf(value), index));
}

JsonDocument JsonDocument::Parse(const std::string &text) {
	JsonDocument parsed;
	// yyjson rejects invalid UTF-8 unless told otherwise (CONTRACTS.md §1).
	// Numbers stay the text they were written as: a parameter value keeps every
	// digit (CONTRACTS.md §3.1), which a double would not.
	parsed.doc = yyjson_read(text.data(), text.size(), YYJSON_READ_NUMBER_AS_RAW);
	if (!parsed.doc) {
		throw std::invalid_argument("not JSON");
	}
	return parsed;
}

JsonDocument::~JsonDocument() {
	yyjson_doc_free(static_cast<yyjson_doc *>(doc));
}

JsonDocument::JsonDocument(JsonDocument &&other) noexcept : doc(other.doc) {
	other.doc = nullptr;
}

JsonDocument &JsonDocument::operator=(JsonDocument &&other) noexcept {
	if (this != &other) {
		yyjson_doc_free(static_cast<yyjson_doc *>(doc));
		doc = other.doc;
		other.doc = nullptr;
	}
	return *this;
}

JsonValue JsonDocument::Root() const {
	return JsonValue(yyjson_doc_get_root(static_cast<yyjson_doc *>(doc)));
}

std::string ErrorBody(const std::string &code, const std::string &message) {
	JsonObject error;
	error.Set("code", code).Set("message", message);
	JsonObject body;
	body.Set("error", error);
	return body.Serialize();
}

std::string ErrorBody(const std::string &code, const std::string &message, const JsonObject &details) {
	JsonObject error;
	error.Set("code", code).Set("message", message).Set("details", details);
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
