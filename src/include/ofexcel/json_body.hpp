#pragma once

// Builds JSON response bodies. Route code uses this instead of yyjson directly,
// so escaping is never written by hand.

#include <cstdint>
#include <memory>
#include <string>

namespace ofexcel {

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

	//! Compact JSON, UTF-8.
	std::string Serialize() const;

private:
	struct Impl;
	std::unique_ptr<Impl> impl;
};

//! {"error":{"code":…,"message":…}} -- CONTRACTS.md §8.1.
std::string ErrorBody(const std::string &code, const std::string &message);

//! Current UTC time as YYYY-MM-DDTHH:MM:SSZ (CONTRACTS.md §1).
std::string UtcNowIso8601();

} // namespace ofexcel
