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

//! {"error":{"code":…,"message":…}} -- CONTRACTS.md §8.1.
std::string ErrorBody(const std::string &code, const std::string &message);

//! Current UTC time as YYYY-MM-DDTHH:MM:SSZ (CONTRACTS.md §1).
std::string UtcNowIso8601();

} // namespace ofexcel
