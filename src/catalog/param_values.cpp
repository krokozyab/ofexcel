#include "ofexcel/param_values.hpp"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace ofexcel {

namespace {

constexpr size_t MAX_STRING_BYTES = 4000;
//! 2^53: the largest integer a JSON number carries exactly.
constexpr double MAX_EXACT_INTEGER = 9007199254740992.0;
//! An exponent beyond this cannot name a value any type here accepts, and
//! expanding it would only build a huge string.
constexpr long MAX_EXPANDED_EXPONENT = 400;

const char *const CODE_MISSING = "E_PARAM_MISSING";
const char *const CODE_TYPE = "E_PARAM_TYPE";
const char *const CODE_TOO_LONG = "E_PARAM_TOO_LONG";

bool IsDigit(char c) {
	return c >= '0' && c <= '9';
}

//! -?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)? (RFC 8259).
bool IsJsonNumber(const std::string &text) {
	size_t i = 0;
	if (i < text.size() && text[i] == '-') {
		i++;
	}
	if (i >= text.size() || !IsDigit(text[i])) {
		return false;
	}
	if (text[i] == '0') {
		i++;
	} else {
		while (i < text.size() && IsDigit(text[i])) {
			i++;
		}
	}
	if (i < text.size() && text[i] == '.') {
		const size_t start = ++i;
		while (i < text.size() && IsDigit(text[i])) {
			i++;
		}
		if (i == start) {
			return false;
		}
	}
	if (i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
		i++;
		if (i < text.size() && (text[i] == '+' || text[i] == '-')) {
			i++;
		}
		const size_t start = i;
		while (i < text.size() && IsDigit(text[i])) {
			i++;
		}
		if (i == start) {
			return false;
		}
	}
	return i == text.size();
}

//! A JSON number as a plain decimal, digit for digit: "1.5e3" → "1500",
//! "1e-7" → "0.0000001". "" if the exponent is out of any useful range.
std::string ExpandExponent(const std::string &number) {
	const auto e = number.find_first_of("eE");
	if (e == std::string::npos) {
		return number;
	}
	const long exponent = std::strtol(number.c_str() + e + 1, nullptr, 10);
	if (exponent > MAX_EXPANDED_EXPONENT || exponent < -MAX_EXPANDED_EXPONENT) {
		return "";
	}
	const bool negative = number[0] == '-';
	const auto mantissa = number.substr(negative ? 1 : 0, e - (negative ? 1 : 0));
	const auto dot = mantissa.find('.');
	const auto integer = mantissa.substr(0, dot);
	const auto fraction = dot == std::string::npos ? std::string() : mantissa.substr(dot + 1);
	const auto digits = integer + fraction;
	const long point = static_cast<long>(integer.size()) + exponent;
	std::string plain;
	if (point <= 0) {
		plain = "0." + std::string(static_cast<size_t>(-point), '0') + digits;
	} else if (point >= static_cast<long>(digits.size())) {
		plain = digits + std::string(static_cast<size_t>(point - static_cast<long>(digits.size())), '0');
	} else {
		plain = digits.substr(0, static_cast<size_t>(point)) + "." + digits.substr(static_cast<size_t>(point));
	}
	return (negative ? "-" : "") + plain;
}

ParamResult Ok(CanonicalValue value) {
	ParamResult result;
	result.ok = true;
	result.value = std::move(value);
	return result;
}

ParamResult Refused(const char *code, std::string message) {
	ParamResult result;
	result.code = code;
	result.message = std::move(message);
	return result;
}

//! A whole JSON number within ±2^53 as integer digits, or "" if it is not one.
std::string ExactInteger(double value) {
	if (std::floor(value) != value || std::fabs(value) > MAX_EXACT_INTEGER) {
		return "";
	}
	char buffer[32];
	std::snprintf(buffer, sizeof(buffer), "%.0f", value);
	const std::string digits(buffer);
	return digits == "-0" ? "0" : digits;
}

ParamResult ScalarFromNumber(const ParamType &type, const std::string &number) {
	if (!IsJsonNumber(number)) {
		return Refused(CODE_TYPE, "\"" + number + "\" is not a JSON number");
	}
	// strtod reads the C locale's decimal point; the daemon never changes the locale.
	const double value = std::strtod(number.c_str(), nullptr);
	if (!std::isfinite(value)) {
		return Refused(CODE_TYPE, "number " + number + " is outside the range of double");
	}
	switch (type.kind) {
	case ParamKind::INTEGER:
	case ParamKind::ID: {
		const auto digits = ExactInteger(value);
		if (digits.empty()) {
			return Refused(CODE_TYPE, "number " + number + " is not an integer within ±2^53");
		}
		if (type.kind == ParamKind::ID && digits[0] == '-') {
			return Refused(CODE_TYPE, "an identifier cannot be negative: " + number);
		}
		return Ok(CanonicalValue::Scalar(digits));
	}
	case ParamKind::NUMBER:
		return Ok(CanonicalValue::Scalar(ShortestDouble(value)));
	case ParamKind::DECIMAL: {
		// The digits as sent, not the double: 0.1 stays 0.1.
		const auto plain = ExpandExponent(number);
		std::string canonical;
		std::string error;
		if (plain.empty() || !CanonicalizeText(type, plain, canonical, error)) {
			return Refused(CODE_TYPE,
			               error.empty() ? "number " + number + " does not fit in " + type.ToString() : error);
		}
		return Ok(CanonicalValue::Scalar(canonical));
	}
	case ParamKind::STRING:
		// A number for a string is written in number's canonical form.
		return Ok(CanonicalValue::Scalar(ShortestDouble(value)));
	case ParamKind::DATE:
	case ParamKind::TIMESTAMP:
		return Refused(CODE_TYPE, "a date is passed as a YYYY-MM-DD string, not as the number " + number +
		                              " (the task pane converts Excel serial dates itself)");
	case ParamKind::BOOLEAN:
	case ParamKind::ENUM:
	case ParamKind::LIST:
		break;
	}
	return Refused(CODE_TYPE, "number " + number + " is not valid for type " + type.ToString());
}

ParamResult ScalarFromString(const ParamType &type, const std::string &text) {
	if (type.kind == ParamKind::STRING && text.size() > MAX_STRING_BYTES) {
		return Refused(CODE_TOO_LONG, "string is longer than " + std::to_string(MAX_STRING_BYTES) + " bytes of UTF-8");
	}
	std::string canonical;
	std::string error;
	if (!CanonicalizeText(type, text, canonical, error)) {
		return Refused(CODE_TYPE, error);
	}
	return Ok(CanonicalValue::Scalar(canonical));
}

ParamResult Scalar(const ParamType &type, const ParamInput &input) {
	switch (input.kind) {
	case ParamInput::Kind::NUMBER:
		return ScalarFromNumber(type, input.text);
	case ParamInput::Kind::STRING:
		return ScalarFromString(type, input.text);
	case ParamInput::Kind::BOOLEAN:
		if (type.kind == ParamKind::BOOLEAN || type.kind == ParamKind::STRING) {
			return Ok(CanonicalValue::Scalar(input.boolean ? "true" : "false"));
		}
		return Refused(CODE_TYPE, std::string("a boolean is not valid for type ") + type.ToString());
	case ParamInput::Kind::ARRAY:
		return Refused(CODE_TYPE, "expected a single value of type " + type.ToString() + ", not a list");
	case ParamInput::Kind::MISSING:
	case ParamInput::Kind::NULL_VALUE:
		break;
	}
	return Refused(CODE_TYPE, "empty value");
}

bool IsEmpty(const ParamInput &input) {
	return input.kind == ParamInput::Kind::MISSING || input.kind == ParamInput::Kind::NULL_VALUE ||
	       (input.kind == ParamInput::Kind::STRING && input.text.empty());
}

} // namespace

ParamInput ParamInput::Missing() {
	return ParamInput();
}

ParamInput ParamInput::Null() {
	ParamInput input;
	input.kind = Kind::NULL_VALUE;
	return input;
}

ParamInput ParamInput::Boolean(bool value) {
	ParamInput input;
	input.kind = Kind::BOOLEAN;
	input.boolean = value;
	return input;
}

ParamInput ParamInput::Number(std::string json_number) {
	ParamInput input;
	input.kind = Kind::NUMBER;
	input.text = std::move(json_number);
	return input;
}

ParamInput ParamInput::String(std::string value) {
	ParamInput input;
	input.kind = Kind::STRING;
	input.text = std::move(value);
	return input;
}

ParamInput ParamInput::Array(std::vector<ParamInput> elements) {
	ParamInput input;
	input.kind = Kind::ARRAY;
	input.elements = std::move(elements);
	return input;
}

ParamResult NormalizeParam(const ParamType &type, const CanonicalValue *default_value, const ParamInput &input) {
	if (IsEmpty(input)) {
		if (default_value) {
			return Ok(*default_value);
		}
		return Refused(CODE_MISSING, "no value given and no default value");
	}
	if (type.kind != ParamKind::LIST) {
		return Scalar(type, input);
	}
	if (input.kind != ParamInput::Kind::ARRAY) {
		return Refused(CODE_TYPE, "expected a list for type " + type.ToString());
	}
	if (input.elements.size() > MAX_LIST_ELEMENTS) {
		return Refused(CODE_TOO_LONG, "the list has more than " + std::to_string(MAX_LIST_ELEMENTS) + " elements");
	}
	std::vector<std::string> elements;
	elements.reserve(input.elements.size());
	for (size_t i = 0; i < input.elements.size(); i++) {
		const auto &element = input.elements[i];
		const auto position = "element " + std::to_string(i + 1) + ": ";
		if (element.kind == ParamInput::Kind::MISSING || element.kind == ParamInput::Kind::NULL_VALUE) {
			return Refused(CODE_TYPE, position + "empty value in the list");
		}
		// "" in a list is a value: a list<string> may hold it, and other types refuse it.
		auto result = Scalar(*type.element, element);
		if (!result.ok) {
			result.message = position + result.message;
			return result;
		}
		elements.push_back(std::move(result.value.scalar));
	}
	return Ok(CanonicalValue::List(std::move(elements)));
}

ParamsResult NormalizeParams(const std::vector<QueryParam> &params, const std::map<std::string, ParamInput> &inputs) {
	ParamsResult result;
	const auto refuse = [&](const char *code, std::string message) {
		result.code = code;
		result.message = std::move(message);
		return result;
	};
	const auto declared = [&](const std::string &name) {
		for (const auto &param : params) {
			if (param.name == name) {
				return true;
			}
		}
		return false;
	};
	// Names match without regard to case, as in the text.
	std::map<std::string, const ParamInput *> by_name;
	for (const auto &input : inputs) {
		auto name = input.first;
		for (auto &c : name) {
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		}
		if (!declared(name)) {
			return refuse("E_PARAM_UNKNOWN", "parameter " + input.first + " is not declared in @param");
		}
		if (!by_name.emplace(name, &input.second).second) {
			return refuse("E_PARAM_UNKNOWN", "parameter " + name + " is passed twice");
		}
	}
	const auto missing = ParamInput::Missing();
	for (const auto &param : params) {
		const auto input = by_name.find(param.name);
		const auto normalized = NormalizeParam(param.type, param.has_default ? &param.default_value : nullptr,
		                                       input == by_name.end() ? missing : *input->second);
		if (!normalized.ok) {
			return refuse(normalized.code.c_str(), "parameter " + param.name + ": " + normalized.message);
		}
		result.values[param.name] = normalized.value;
	}
	result.ok = true;
	return result;
}

} // namespace ofexcel
