#include "ofexcel/contract_types.hpp"

#include "ofexcel/utf8.hpp"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace ofexcel {

namespace {

constexpr size_t MAX_STRING_BYTES = 4000;
constexpr size_t MAX_DIGITS = 38;

bool IsDigit(char c) {
	return c >= '0' && c <= '9';
}

bool AllDigits(const std::string &text, size_t from = 0, size_t to = std::string::npos) {
	to = to == std::string::npos ? text.size() : to;
	if (from >= to) {
		return false;
	}
	for (size_t i = from; i < to; i++) {
		if (!IsDigit(text[i])) {
			return false;
		}
	}
	return true;
}

std::string LowerAscii(std::string text) {
	for (auto &c : text) {
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	return text;
}

std::string StripLeadingZeros(const std::string &digits) {
	const auto first = digits.find_first_not_of('0');
	return first == std::string::npos ? "0" : digits.substr(first);
}

// --- Type grammar -------------------------------------------------------------

//! Words ([A-Za-z0-9_]+) and the punctuation ( ) , < >, with whitespace between
//! them dropped. Anything else is an error.
bool Lex(const std::string &text, std::vector<std::string> &tokens, std::string &error) {
	size_t i = 0;
	while (i < text.size()) {
		const char c = text[i];
		if (c == ' ' || c == '\t') {
			i++;
		} else if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
			size_t end = i;
			while (end < text.size() && (std::isalnum(static_cast<unsigned char>(text[end])) || text[end] == '_')) {
				end++;
			}
			tokens.push_back(text.substr(i, end - i));
			i = end;
		} else if (c == '(' || c == ')' || c == ',' || c == '<' || c == '>') {
			tokens.emplace_back(1, c);
			i++;
		} else {
			error = std::string("invalid character \"") + c + "\" in type \"" + text + "\"";
			return false;
		}
	}
	if (tokens.empty()) {
		error = "type is missing";
		return false;
	}
	return true;
}

bool IsWord(const std::string &token) {
	return !token.empty() && (std::isalnum(static_cast<unsigned char>(token[0])) || token[0] == '_');
}

struct TokenCursor {
	const std::vector<std::string> &tokens;
	size_t at = 0;

	bool Done() const {
		return at >= tokens.size();
	}
	const std::string &Peek() const {
		static const std::string END;
		return Done() ? END : tokens[at];
	}
	bool Take(const char *expected) {
		if (Peek() == expected) {
			at++;
			return true;
		}
		return false;
	}
};

bool ParseSmallInt(TokenCursor &cursor, int &value) {
	const auto &token = cursor.Peek();
	if (!AllDigits(token) || token.size() > 3) {
		return false;
	}
	value = std::stoi(token);
	cursor.at++;
	return true;
}

//! decimal(p,s) after the word "decimal".
bool ParseDecimalArguments(TokenCursor &cursor, int &precision, int &scale, std::string &error) {
	if (!cursor.Take("(") || !ParseSmallInt(cursor, precision) || !cursor.Take(",") || !ParseSmallInt(cursor, scale) ||
	    !cursor.Take(")")) {
		error = "decimal is written as decimal(p,s), for example decimal(18,2)";
		return false;
	}
	if (precision < 1 || precision > 38 || scale < 0 || scale > precision) {
		error = "decimal(p,s) requires 1 ≤ p ≤ 38 and 0 ≤ s ≤ p, not decimal(" + std::to_string(precision) + "," +
		        std::to_string(scale) + ")";
		return false;
	}
	return true;
}

bool ParseParamTypeAt(TokenCursor &cursor, ParamType &type, bool scalar_only, std::string &error) {
	const auto word = cursor.Peek();
	cursor.at++;
	static const struct {
		const char *name;
		ParamKind kind;
	} SIMPLE[] = {{"integer", ParamKind::INTEGER}, {"number", ParamKind::NUMBER},       {"string", ParamKind::STRING},
	              {"id", ParamKind::ID},           {"date", ParamKind::DATE},           {"timestamp", ParamKind::TIMESTAMP},
	              {"boolean", ParamKind::BOOLEAN}};
	for (const auto &simple : SIMPLE) {
		if (word == simple.name) {
			type.kind = simple.kind;
			return true;
		}
	}
	if (word == "decimal") {
		type.kind = ParamKind::DECIMAL;
		return ParseDecimalArguments(cursor, type.precision, type.scale, error);
	}
	if (word == "enum" || word == "list") {
		if (scalar_only) {
			error = "only a scalar type is allowed inside list<…>: enum and list in list are not supported";
			return false;
		}
	}
	if (word == "enum") {
		type.kind = ParamKind::ENUM;
		if (!cursor.Take("(")) {
			error = "enum is written as enum(A,B,…)";
			return false;
		}
		do {
			const auto &variant = cursor.Peek();
			if (!IsWord(variant)) {
				error = "enum variants are words of letters, digits and _, separated by commas";
				return false;
			}
			for (const auto &existing : type.variants) {
				if (LowerAscii(existing) == LowerAscii(variant)) {
					error = "enum variant \"" + variant + "\" is repeated (variants are compared case-insensitively)";
					return false;
				}
			}
			type.variants.push_back(variant);
			cursor.at++;
		} while (cursor.Take(","));
		if (!cursor.Take(")")) {
			error = "enum is written as enum(A,B,…)";
			return false;
		}
		return true;
	}
	if (word == "list") {
		type.kind = ParamKind::LIST;
		auto element = std::make_shared<ParamType>();
		if (!cursor.Take("<") || cursor.Done()) {
			error = "list is written as list<type>, for example list<id>";
			return false;
		}
		if (!ParseParamTypeAt(cursor, *element, true, error)) {
			return false;
		}
		if (!cursor.Take(">")) {
			error = "list is written as list<type>, for example list<id>";
			return false;
		}
		type.element = element;
		return true;
	}
	error = "unknown type \"" + word + "\"";
	return false;
}

// --- Values -------------------------------------------------------------------

bool IsLeapYear(int year) {
	return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

//! YYYY-MM-DD at `text[from]`, a date that exists, years 0001–9999.
bool IsValidDate(const std::string &text, size_t from) {
	if (text.size() < from + 10 || text[from + 4] != '-' || text[from + 7] != '-' || !AllDigits(text, from, from + 4) ||
	    !AllDigits(text, from + 5, from + 7) || !AllDigits(text, from + 8, from + 10)) {
		return false;
	}
	const int year = std::stoi(text.substr(from, 4));
	const int month = std::stoi(text.substr(from + 5, 2));
	const int day = std::stoi(text.substr(from + 8, 2));
	static const int DAYS[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
	if (year < 1 || month < 1 || month > 12 || day < 1) {
		return false;
	}
	const int last = DAYS[month - 1] + (month == 2 && IsLeapYear(year) ? 1 : 0);
	return day <= last;
}

bool CanonicalTimestamp(const std::string &text, std::string &canonical) {
	// YYYY-MM-DD[ T]HH:MM:SS[.f{1,6}]
	if (text.size() < 19 || !IsValidDate(text, 0) || (text[10] != ' ' && text[10] != 'T') || text[13] != ':' ||
	    text[16] != ':' || !AllDigits(text, 11, 13) || !AllDigits(text, 14, 16) || !AllDigits(text, 17, 19)) {
		return false;
	}
	if (std::stoi(text.substr(11, 2)) > 23 || std::stoi(text.substr(14, 2)) > 59 || std::stoi(text.substr(17, 2)) > 59) {
		return false;
	}
	std::string fraction;
	if (text.size() > 19) {
		if (text[19] != '.' || text.size() < 21 || text.size() > 26 || !AllDigits(text, 20)) {
			return false;
		}
		fraction = text.substr(20);
		while (!fraction.empty() && fraction.back() == '0') {
			fraction.pop_back();
		}
	}
	canonical = text.substr(0, 10) + " " + text.substr(11, 8) + (fraction.empty() ? "" : "." + fraction);
	return true;
}

bool CanonicalDecimal(const ParamType &type, const std::string &text, std::string &canonical, std::string &error) {
	if (!IsPlainDecimal(text)) {
		error = "\"" + text + "\" is not a decimal number";
		return false;
	}
	canonical = CanonicalDecimalText(text);
	const auto body = canonical[0] == '-' ? canonical.substr(1) : canonical;
	const auto dot = body.find('.');
	const size_t integer_digits = body == "0" || body.compare(0, 2, "0.") == 0 ? 0 : body.substr(0, dot).size();
	const size_t fraction_digits = dot == std::string::npos ? 0 : body.size() - dot - 1;
	// As DECIMAL(p,s) in SQL: p-s digits before the point, s after it.
	if (integer_digits > static_cast<size_t>(type.precision - type.scale) ||
	    fraction_digits > static_cast<size_t>(type.scale)) {
		error = "\"" + text + "\" does not fit in " + type.ToString() + ": at most " +
		        std::to_string(type.precision - type.scale) + " digits before the point and " +
		        std::to_string(type.scale) + " after it";
		return false;
	}
	return true;
}

bool CanonicalNumber(const std::string &text, std::string &canonical, std::string &error) {
	if (!IsPlainDecimal(text)) {
		error = "\"" + text + "\" is not a number in decimal notation (no exponent allowed)";
		return false;
	}
	// strtod reads the C locale's decimal point; the daemon never changes the locale.
	const double value = std::strtod(text.c_str(), nullptr);
	if (!std::isfinite(value)) {
		error = "\"" + text + "\" is outside the range of double";
		return false;
	}
	canonical = ShortestDouble(value);
	return true;
}

bool CanonicalString(const std::string &text, std::string &error) {
	if (text.size() > MAX_STRING_BYTES) {
		error = "string is longer than " + std::to_string(MAX_STRING_BYTES) + " bytes of UTF-8";
		return false;
	}
	if (!IsValidUtf8(text)) {
		error = "string is not valid UTF-8";
		return false;
	}
	for (const char c : text) {
		const auto byte = static_cast<unsigned char>(c);
		if (byte < 0x20 && byte != '\t' && byte != '\n' && byte != '\r') {
			error = "string contains control character U+00" + std::string(1, "0123456789ABCDEF"[byte >> 4]) +
			        "0123456789ABCDEF"[byte & 0x0f];
			return false;
		}
	}
	return true;
}

} // namespace

bool IsDateAt(const std::string &text, size_t from) {
	return IsValidDate(text, from);
}

bool IsPlainDecimal(const std::string &text) {
	size_t i = text.empty() || text[0] != '-' ? 0 : 1;
	size_t int_digits = 0;
	while (i < text.size() && IsDigit(text[i])) {
		i++;
		int_digits++;
	}
	size_t frac_digits = 0;
	if (i < text.size() && text[i] == '.') {
		i++;
		while (i < text.size() && IsDigit(text[i])) {
			i++;
			frac_digits++;
		}
	}
	return i == text.size() && int_digits + frac_digits > 0;
}

std::string CanonicalDecimalText(const std::string &plain) {
	const bool negative = !plain.empty() && plain[0] == '-';
	const auto body = plain.substr(negative ? 1 : 0);
	const auto dot = body.find('.');
	auto integer = body.substr(0, dot);
	auto fraction = dot == std::string::npos ? std::string() : body.substr(dot + 1);
	integer = integer.find_first_not_of('0') == std::string::npos ? "" : StripLeadingZeros(integer);
	while (!fraction.empty() && fraction.back() == '0') {
		fraction.pop_back();
	}
	const bool zero = integer.empty() && fraction.empty();
	return (negative && !zero ? "-" : "") + (integer.empty() ? "0" : integer) + (fraction.empty() ? "" : "." + fraction);
}

std::string ParamType::ToString() const {
	switch (kind) {
	case ParamKind::INTEGER:
		return "integer";
	case ParamKind::NUMBER:
		return "number";
	case ParamKind::DECIMAL:
		return "decimal(" + std::to_string(precision) + "," + std::to_string(scale) + ")";
	case ParamKind::STRING:
		return "string";
	case ParamKind::ID:
		return "id";
	case ParamKind::DATE:
		return "date";
	case ParamKind::TIMESTAMP:
		return "timestamp";
	case ParamKind::BOOLEAN:
		return "boolean";
	case ParamKind::ENUM: {
		std::string text = "enum(";
		for (size_t i = 0; i < variants.size(); i++) {
			text += (i ? "," : "") + variants[i];
		}
		return text + ")";
	}
	case ParamKind::LIST:
		return "list<" + (element ? element->ToString() : std::string("?")) + ">";
	}
	return "?";
}

std::string ColumnType::ToString() const {
	switch (kind) {
	case ColumnKind::ID:
		return "id";
	case ColumnKind::INTEGER:
		return "integer";
	case ColumnKind::DECIMAL:
		return "decimal(" + std::to_string(precision) + "," + std::to_string(scale) + ")";
	case ColumnKind::NUMBER:
		return "number";
	case ColumnKind::STRING:
		return "string";
	case ColumnKind::TEXT:
		return "text";
	case ColumnKind::DATE:
		return "date";
	case ColumnKind::TIMESTAMP:
		return "timestamp";
	case ColumnKind::TIMESTAMPTZ:
		return "timestamptz";
	case ColumnKind::BOOLEAN:
		return "boolean";
	}
	return "?";
}

bool ParseParamType(const std::string &text, ParamType &type, std::string &error) {
	std::vector<std::string> tokens;
	if (!Lex(text, tokens, error)) {
		return false;
	}
	TokenCursor cursor {tokens};
	type = ParamType();
	if (!ParseParamTypeAt(cursor, type, false, error)) {
		return false;
	}
	if (!cursor.Done()) {
		error = "unexpected \"" + cursor.Peek() + "\" after type " + type.ToString();
		return false;
	}
	return true;
}

bool ParseColumnType(const std::string &text, ColumnType &type, std::string &error) {
	std::vector<std::string> tokens;
	if (!Lex(text, tokens, error)) {
		return false;
	}
	TokenCursor cursor {tokens};
	type = ColumnType();
	const auto word = cursor.Peek();
	cursor.at++;
	static const struct {
		const char *name;
		ColumnKind kind;
	} SIMPLE[] = {{"id", ColumnKind::ID},
	              {"integer", ColumnKind::INTEGER},
	              {"number", ColumnKind::NUMBER},
	              {"string", ColumnKind::STRING},
	              {"text", ColumnKind::TEXT},
	              {"date", ColumnKind::DATE},
	              {"timestamp", ColumnKind::TIMESTAMP},
	              {"timestamptz", ColumnKind::TIMESTAMPTZ},
	              {"boolean", ColumnKind::BOOLEAN}};
	bool known = false;
	for (const auto &simple : SIMPLE) {
		if (word == simple.name) {
			type.kind = simple.kind;
			known = true;
		}
	}
	if (word == "decimal") {
		type.kind = ColumnKind::DECIMAL;
		if (!ParseDecimalArguments(cursor, type.precision, type.scale, error)) {
			return false;
		}
		known = true;
	}
	if (!known) {
		error = "unknown column type \"" + word + "\"";
		return false;
	}
	if (!cursor.Done()) {
		error = "unexpected \"" + cursor.Peek() + "\" after type " + type.ToString();
		return false;
	}
	return true;
}

bool CanonicalizeText(const ParamType &type, const std::string &text, std::string &canonical, std::string &error) {
	switch (type.kind) {
	case ParamKind::INTEGER: {
		const bool negative = !text.empty() && text[0] == '-';
		const size_t from = negative ? 1 : 0;
		if (!AllDigits(text, from) || text.size() - from > MAX_DIGITS) {
			error = "\"" + text + "\" is not an integer";
			return false;
		}
		const auto digits = StripLeadingZeros(text.substr(from));
		canonical = (negative && digits != "0" ? "-" : "") + digits;
		return true;
	}
	case ParamKind::ID:
		if (!AllDigits(text) || text.size() > MAX_DIGITS) {
			error = "\"" + text + "\" is not an identifier of digits (up to 38)";
			return false;
		}
		canonical = StripLeadingZeros(text);
		return true;
	case ParamKind::NUMBER:
		return CanonicalNumber(text, canonical, error);
	case ParamKind::DECIMAL:
		return CanonicalDecimal(type, text, canonical, error);
	case ParamKind::STRING:
		if (!CanonicalString(text, error)) {
			return false;
		}
		canonical = text;
		return true;
	case ParamKind::DATE:
		if (text.size() != 10 || !IsValidDate(text, 0)) {
			error = "\"" + text + "\" is not a valid date YYYY-MM-DD";
			return false;
		}
		canonical = text;
		return true;
	case ParamKind::TIMESTAMP:
		if (!CanonicalTimestamp(text, canonical)) {
			error = "\"" + text + "\" is not a timestamp YYYY-MM-DD HH:MM:SS[.ffffff]";
			return false;
		}
		return true;
	case ParamKind::BOOLEAN: {
		const auto lower = LowerAscii(text);
		if (lower == "true" || lower == "y" || lower == "1") {
			canonical = "true";
			return true;
		}
		if (lower == "false" || lower == "n" || lower == "0") {
			canonical = "false";
			return true;
		}
		error = "\"" + text + "\" is not a boolean (true/false, Y/N, 1/0)";
		return false;
	}
	case ParamKind::ENUM:
		for (const auto &variant : type.variants) {
			if (LowerAscii(variant) == LowerAscii(text)) {
				canonical = variant;
				return true;
			}
		}
		error = "\"" + text + "\" is not one of " + type.ToString();
		return false;
	case ParamKind::LIST:
		error = "list<…> cannot be converted as a scalar value";
		return false;
	}
	return false;
}

std::string ShortestDouble(double value) {
	if (value == 0) {
		return "0";
	}
	// The fewest significant digits that read back as `value`: what Ryu and
	// std::to_chars find, and with correctly rounded %.*e the same digits.
	char buffer[40];
	for (int precision = 1; precision <= 17; precision++) {
		std::snprintf(buffer, sizeof(buffer), "%.*e", precision - 1, value);
		if (std::strtod(buffer, nullptr) == value) {
			break;
		}
	}
	std::string text(buffer);
	const bool negative = text[0] == '-';
	const auto e = text.find('e');
	const int exponent = std::atoi(text.c_str() + e + 1);
	std::string digits;
	for (size_t i = negative ? 1 : 0; i < e; i++) {
		if (IsDigit(text[i])) {
			digits += text[i];
		}
	}
	while (digits.size() > 1 && digits.back() == '0') {
		digits.pop_back();
	}
	const int count = static_cast<int>(digits.size());

	std::string scientific = digits.substr(0, 1) + (count > 1 ? "." + digits.substr(1) : "") + "e" +
	                         (exponent < 0 ? "-" : "+") + (std::abs(exponent) < 10 ? "0" : "") +
	                         std::to_string(std::abs(exponent));
	std::string fixed;
	if (exponent < 0) {
		fixed = "0." + std::string(static_cast<size_t>(-exponent - 1), '0') + digits;
	} else if (count - 1 <= exponent) {
		fixed = digits + std::string(static_cast<size_t>(exponent - (count - 1)), '0');
	} else {
		fixed = digits.substr(0, static_cast<size_t>(exponent + 1)) + "." + digits.substr(static_cast<size_t>(exponent + 1));
	}
	return (negative ? "-" : "") + (fixed.size() <= scientific.size() ? fixed : scientific);
}

} // namespace ofexcel
