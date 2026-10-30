#pragma once

// Builds JSON response bodies. Route code uses this instead of yyjson directly,
// so escaping is never written by hand.

#include <cstdint>
#include <memory>
#include <string>

namespace ofexcel {

class JsonObject;

//! An array inside a JsonObject's document, filled in place: nothing is copied,
//! which matters for a page of result rows. Valid only while the JsonObject that
//! handed it out is alive. Methods are named by type so that AddInt(5) cannot
//! silently become a boolean.
class JsonArrayRef {
public:
	JsonArrayRef &AddString(const std::string &value);
	JsonArrayRef &AddInt(int64_t value);
	JsonArrayRef &AddDouble(double value);
	JsonArrayRef &AddBool(bool value);
	JsonArrayRef &AddNull();
	//! Copies `value` in; `value` stays usable.
	JsonArrayRef &AddObject(const JsonObject &value);
	//! A nested array, appended now and filled through the returned reference.
	JsonArrayRef AddArray();

private:
	friend class JsonObject;
	JsonArrayRef(void *doc, void *array) : doc(doc), array(array) {
	}
	void *doc;
	void *array;
};

//! A JSON object built key by key, in insertion order.
class JsonObject {
public:
	JsonObject();
	~JsonObject();
	JsonObject(JsonObject &&) noexcept;
	JsonObject &operator=(JsonObject &&) noexcept;
	JsonObject(const JsonObject &) = delete;
	JsonObject &operator=(const JsonObject &) = delete;

	JsonObject &Set(const std::string &key, const std::string &value);
	JsonObject &Set(const std::string &key, const char *value);
	JsonObject &Set(const std::string &key, int64_t value);
	JsonObject &Set(const std::string &key, bool value);
	JsonObject &SetNull(const std::string &key);
	//! Copies `value` in; `value` stays usable.
	JsonObject &Set(const std::string &key, const JsonObject &value);
	//! An empty array under `key`, filled through the returned reference.
	JsonArrayRef SetArray(const std::string &key);

	//! Compact JSON, UTF-8.
	std::string Serialize() const;

private:
	friend class JsonArrayRef;
	struct Impl;
	std::unique_ptr<Impl> impl;
};

enum class JsonKind { MISSING, NULL_VALUE, BOOLEAN, NUMBER, STRING, ARRAY, OBJECT };

//! A value inside a JsonDocument. Valid only while that document is alive.
//! Reading a member that is not there, or a member of something that is not
//! an object, gives a MISSING value rather than throwing, so a route checks the
//! kind once instead of guarding every step.
class JsonValue {
public:
	JsonKind Kind() const;
	bool Is(JsonKind kind) const {
		return Kind() == kind;
	}
	//! The string, which may contain NUL; "" unless Kind() is STRING.
	std::string String() const;
	//! false unless Kind() is BOOLEAN.
	bool Bool() const;
	JsonValue Get(const std::string &key) const;
	//! Elements of an array; 0 for anything else.
	size_t Size() const;
	//! Element `index` of an array; MISSING past the end or for anything else.
	JsonValue At(size_t index) const;

private:
	friend class JsonDocument;
	explicit JsonValue(void *value) : value(value) {
	}
	void *value;
};

//! A request body, parsed and read-only.
class JsonDocument {
public:
	//! Throws std::invalid_argument when `text` is not JSON or not valid UTF-8.
	static JsonDocument Parse(const std::string &text);

	~JsonDocument();
	JsonDocument(JsonDocument &&other) noexcept;
	JsonDocument &operator=(JsonDocument &&other) noexcept;
	JsonDocument(const JsonDocument &) = delete;
	JsonDocument &operator=(const JsonDocument &) = delete;

	JsonValue Root() const;

private:
	JsonDocument() = default;
	void *doc = nullptr;
};

//! {"error":{"code":…,"message":…}} -- CONTRACTS.md §8.1.
std::string ErrorBody(const std::string &code, const std::string &message);
//! The same with `details` copied in under error.details.
std::string ErrorBody(const std::string &code, const std::string &message, const JsonObject &details);

//! Current UTC time as YYYY-MM-DDTHH:MM:SSZ (CONTRACTS.md §1).
std::string UtcNowIso8601();

} // namespace ofexcel
