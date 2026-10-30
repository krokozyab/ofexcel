#include "ofexcel/fusion_errors.hpp"

#include <iostream>
#include <string>

namespace ofexcel {

namespace {

constexpr const char *REJECTED_PREFIX = "Oracle Fusion rejected the query";

bool StartsWith(const std::string &text, const char *prefix) {
	return text.rfind(prefix, 0) == 0;
}

//! ofquack appends the statement after a newline; the user needs the reason.
std::string FirstLine(const std::string &text) {
	return text.substr(0, text.find('\n'));
}

} // namespace

bool IsFusionNoRows(const duckdb::ErrorData &error) {
	return error.Type() == duckdb::ExceptionType::IO && StartsWith(error.RawMessage(), FUSION_NO_ROWS_PREFIX);
}

ApiError FusionApiError(const duckdb::ErrorData &error) {
	const auto &raw = error.RawMessage();
	std::cerr << "ofexceld: Fusion error: " << raw << std::endl;
	switch (error.Type()) {
	case duckdb::ExceptionType::INVALID_INPUT:
		// ofquack's authentication failures, including "Not signed in".
		return ApiError("E_FUSION_AUTH", 409,
		                "Нет входа в Oracle Fusion или срок входа истёк. Войдите снова: " + FirstLine(raw));
	case duckdb::ExceptionType::IO:
		if (StartsWith(raw, REJECTED_PREFIX)) {
			return ApiError("E_FUSION_REJECTED", 502, "Oracle Fusion отклонил запрос: " + FirstLine(raw));
		}
		return ApiError("E_FUSION_UNAVAILABLE", 503, "Oracle Fusion недоступен: " + FirstLine(raw));
	case duckdb::ExceptionType::OUT_OF_MEMORY:
		return ApiError("E_OUT_OF_MEMORY", 507, "Не хватает памяти DuckDB: " + FirstLine(raw));
	default:
		return ApiError("E_INTERNAL", 500, "Внутренняя ошибка ofexceld: " + FirstLine(raw));
	}
}

} // namespace ofexcel
