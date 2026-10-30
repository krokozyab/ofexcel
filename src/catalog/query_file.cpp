#include "ofexcel/query_file.hpp"

#include "ofexcel/utf8.hpp"

#include <algorithm>
#include <cctype>
#include <map>

namespace ofexcel {

namespace {

constexpr size_t MAX_TITLE_CHARACTERS = 200;
constexpr int64_t MAX_VERSION = 2147483647;
constexpr int64_t MAX_MAX_ROWS = 1048575;

const char *const CODE_PARSE = "E_QUERY_PARSE";
const char *const CODE_UNSUPPORTED = "E_UNSUPPORTED";

const char *const DIRECTIVES[] = {"id",     "engine", "version", "title",   "description", "connection",
                                  "param",  "column", "depends", "refresh", "max_rows"};
const char *const REPEATABLE[] = {"description", "param", "column", "depends"};

bool IsSpace(char c) {
	return c == ' ' || c == '\t';
}

std::string Trim(const std::string &text) {
	size_t begin = 0;
	size_t end = text.size();
	while (begin < end && IsSpace(text[begin])) {
		begin++;
	}
	while (end > begin && IsSpace(text[end - 1])) {
		end--;
	}
	return text.substr(begin, end - begin);
}

std::string Lower(std::string text) {
	for (auto &c : text) {
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	return text;
}

bool IsAsciiLetter(char c) {
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool IsDigit(char c) {
	return c >= '0' && c <= '9';
}

bool Contains(const char *const *list, size_t size, const std::string &value) {
	for (size_t i = 0; i < size; i++) {
		if (value == list[i]) {
			return true;
		}
	}
	return false;
}

//! [A-Za-z][A-Za-z0-9_$#]* (§2.2 colname).
bool IsColumnName(const std::string &text) {
	if (text.empty() || !IsAsciiLetter(text[0])) {
		return false;
	}
	for (const char c : text) {
		if (!IsAsciiLetter(c) && !IsDigit(c) && c != '_' && c != '$' && c != '#') {
			return false;
		}
	}
	return true;
}

//! A positive integer within `max`, digits only.
bool ParseBoundedInteger(const std::string &text, int64_t max, int64_t &value) {
	if (text.empty() || text.size() > 10) {
		return false;
	}
	for (const char c : text) {
		if (!IsDigit(c)) {
			return false;
		}
	}
	value = std::stoll(text);
	return value >= 1 && value <= max;
}

//! Scans one argument list of @depends. Positions are byte offsets into `text`.
class DependencyScanner {
public:
	explicit DependencyScanner(const std::string &text) : text(text) {
	}

	bool Parse(Dependency &dependency, std::string &error) {
		SkipSpaces();
		const auto name = Word();
		if (name.empty()) {
			error = "@depends: expected query_id(…) or xl_import.name";
			return false;
		}
		if (name == "xl_import" && Take('.')) {
			dependency.kind = Dependency::Kind::IMPORT;
			dependency.name = Word();
			if (!IsIdentifier(dependency.name)) {
				error = "@depends: invalid import name \"" + dependency.name + "\" after xl_import.";
				return false;
			}
			return End(error);
		}
		dependency.kind = Dependency::Kind::QUERY;
		dependency.name = name;
		if (!IsIdentifier(name)) {
			error = "@depends: invalid query_id \"" + name + "\"";
			return false;
		}
		SkipSpaces();
		if (!Take('(')) {
			error = "@depends: expected \"(\" after " + name + "; with no arguments, write " + name + "()";
			return false;
		}
		SkipSpaces();
		if (Take(')')) {
			return End(error);
		}
		do {
			DependencyArgument argument;
			if (!Argument(argument, error)) {
				return false;
			}
			for (const auto &existing : dependency.arguments) {
				if (existing.name == argument.name) {
					error = "@depends: argument " + argument.name + " is repeated";
					return false;
				}
			}
			dependency.arguments.push_back(std::move(argument));
			SkipSpaces();
		} while (Take(','));
		if (!Take(')')) {
			error = "@depends: expected \",\" or \")\"";
			return false;
		}
		return End(error);
	}

private:
	bool Argument(DependencyArgument &argument, std::string &error) {
		SkipSpaces();
		argument.name = Word();
		if (!IsIdentifier(argument.name)) {
			error = "@depends: invalid argument name \"" + argument.name + "\"";
			return false;
		}
		SkipSpaces();
		if (!Take(':') || !Take('=')) {
			error = "@depends: expected \":=\" after " + argument.name;
			return false;
		}
		SkipSpaces();
		if (Take('$')) {
			argument.kind = DependencyArgument::Kind::PARAM;
			argument.value = Word();
			if (!IsIdentifier(argument.value)) {
				error = "@depends: expected a parameter name after $";
				return false;
			}
			return true;
		}
		if (Peek() == '\'') {
			argument.kind = DependencyArgument::Kind::STRING;
			return Quoted(argument.value, error);
		}
		if (Peek() == '-' || IsDigit(Peek())) {
			argument.kind = DependencyArgument::Kind::NUMBER;
			return Number(argument.value, error);
		}
		const auto word = Lower(Word());
		if (word == "true" || word == "false") {
			argument.kind = DependencyArgument::Kind::BOOLEAN;
			argument.value = word;
			return true;
		}
		if (word == "date") {
			argument.kind = DependencyArgument::Kind::DATE;
			SkipSpaces();
			std::string date;
			if (Peek() != '\'' || !Quoted(date, error)) {
				error = "@depends: DATE is written as DATE 'YYYY-MM-DD'";
				return false;
			}
			ParamType type;
			type.kind = ParamKind::DATE;
			std::string canonical;
			if (!CanonicalizeText(type, date, canonical, error)) {
				error = "@depends: " + error;
				return false;
			}
			argument.value = canonical;
			return true;
		}
		error = "@depends: the value of argument " + argument.name +
		        " must be a $parameter, 'string', number, DATE 'YYYY-MM-DD', TRUE or FALSE";
		return false;
	}

	bool Quoted(std::string &value, std::string &error) {
		at++; // the opening quote
		while (at < text.size()) {
			if (text[at] == '\'') {
				if (at + 1 < text.size() && text[at + 1] == '\'') {
					value += '\'';
					at += 2;
					continue;
				}
				at++;
				return true;
			}
			value += text[at++];
		}
		error = "@depends: unterminated string literal";
		return false;
	}

	//! -?[0-9]+(\.[0-9]+)?
	bool Number(std::string &value, std::string &error) {
		const size_t start = at;
		Take('-');
		const size_t integer_start = at;
		while (IsDigit(Peek())) {
			at++;
		}
		bool valid = at > integer_start;
		if (valid && Take('.')) {
			const size_t fraction_start = at;
			while (IsDigit(Peek())) {
				at++;
			}
			valid = at > fraction_start;
		}
		if (!valid) {
			error = "@depends: invalid number";
			return false;
		}
		value = text.substr(start, at - start);
		return true;
	}

	bool End(std::string &error) {
		SkipSpaces();
		if (at != text.size()) {
			error = "@depends: unexpected text \"" + text.substr(at) + "\"";
			return false;
		}
		return true;
	}

	std::string Word() {
		const size_t start = at;
		while (at < text.size() && (std::isalnum(static_cast<unsigned char>(text[at])) || text[at] == '_')) {
			at++;
		}
		return text.substr(start, at - start);
	}

	void SkipSpaces() {
		while (at < text.size() && IsSpace(text[at])) {
			at++;
		}
	}

	char Peek() const {
		return at < text.size() ? text[at] : '\0';
	}

	bool Take(char c) {
		if (Peek() == c) {
			at++;
			return true;
		}
		return false;
	}

	const std::string &text;
	size_t at = 0;
};

//! True if `body` holds something besides whitespace and SQL comments. A
//! string literal cannot fool it: the first character that is neither is the answer.
bool HasStatement(const std::string &body) {
	size_t i = 0;
	while (i < body.size()) {
		const char c = body[i];
		if (IsSpace(c) || c == '\n' || c == '\r' || c == '\f' || c == '\v') {
			i++;
		} else if (body.compare(i, 2, "--") == 0) {
			const auto end = body.find('\n', i);
			i = end == std::string::npos ? body.size() : end + 1;
		} else if (body.compare(i, 2, "/*") == 0) {
			const auto end = body.find("*/", i + 2);
			// An unclosed comment holds no statement; the tokenizer reports it.
			i = end == std::string::npos ? body.size() : end + 2;
		} else {
			return true;
		}
	}
	return false;
}

class HeaderParser {
public:
	ParsedQueryFile Parse(const std::string &text) {
		const auto normalized = NormalizeDefinitionText(text);
		std::vector<std::string> lines;
		std::vector<size_t> offsets;
		size_t start = 0;
		while (true) {
			const auto end = normalized.find('\n', start);
			offsets.push_back(start);
			lines.push_back(normalized.substr(start, end == std::string::npos ? std::string::npos : end - start));
			if (end == std::string::npos) {
				break;
			}
			start = end + 1;
		}

		size_t header_end = 0;
		while (header_end < lines.size()) {
			const auto trimmed = Trim(lines[header_end]);
			if (!trimmed.empty() && trimmed.compare(0, 2, "--") != 0) {
				break;
			}
			header_end++;
		}
		for (size_t i = 0; i < header_end; i++) {
			const int line = static_cast<int>(i) + 1;
			if (!IsValidUtf8(lines[i])) {
				Error(line, "line is not valid UTF-8");
				continue;
			}
			HeaderLine(line, Trim(lines[i]));
		}

		auto &header = result.header;
		header.body_line = static_cast<int>(header_end) + 1;
		header.body = header_end < lines.size() ? normalized.substr(offsets[header_end]) : std::string();
		CheckWholeHeader();
		if (!HasStatement(header.body)) {
			Error(header.body_line, "no query text after the header");
		}
		return std::move(result);
	}

private:
	void Error(int line, const std::string &message, const char *code = CODE_PARSE) {
		QueryParseError error;
		error.line = line;
		error.code = code;
		error.message = message;
		result.errors.push_back(std::move(error));
	}

	void HeaderLine(int line, const std::string &trimmed) {
		// "--", spaces, "@" and a letter: a directive. Anything else in the
		// header is a comment.
		size_t at = 2;
		while (at < trimmed.size() && IsSpace(trimmed[at])) {
			at++;
		}
		if (at + 1 >= trimmed.size() || trimmed[at] != '@' || !IsAsciiLetter(trimmed[at + 1])) {
			return;
		}
		at++;
		const size_t key_start = at;
		while (at < trimmed.size() && (std::isalnum(static_cast<unsigned char>(trimmed[at])) || trimmed[at] == '_')) {
			at++;
		}
		const auto key = trimmed.substr(key_start, at - key_start);
		// "@key: value" or "@key value": @param and @column carry a colon of their own.
		while (at < trimmed.size() && IsSpace(trimmed[at])) {
			at++;
		}
		if (at < trimmed.size() && trimmed[at] == ':') {
			at++;
		}
		const auto value = Trim(trimmed.substr(at));

		if (!Contains(DIRECTIVES, sizeof(DIRECTIVES) / sizeof(*DIRECTIVES), key)) {
			const auto lower = Lower(key);
			if (lower != key && Contains(DIRECTIVES, sizeof(DIRECTIVES) / sizeof(*DIRECTIVES), lower)) {
				Error(line, "directives are written in lowercase: @" + lower + ", not @" + key);
			} else {
				Error(line, "unknown directive @" + key);
			}
			return;
		}
		if (!Contains(REPEATABLE, sizeof(REPEATABLE) / sizeof(*REPEATABLE), key)) {
			const auto seen = first_line.find(key);
			if (seen != first_line.end()) {
				Error(line, "directive @" + key + " is repeated (first on line " + std::to_string(seen->second) + ")");
				return;
			}
			first_line[key] = line;
		}
		if (value.empty() && key != "description") {
			Error(line, "directive @" + key + " has no value");
			return;
		}
		Directive(line, key, value);
	}

	void Directive(int line, const std::string &key, const std::string &value) {
		auto &header = result.header;
		int64_t number = 0;
		if (key == "id") {
			if (!IsIdentifier(value)) {
				Error(line, "@id: \"" + value + "\" is not an identifier: [a-z][a-z0-9_], up to 63 characters");
				return;
			}
			header.id = value;
		} else if (key == "engine") {
			if (value == "oracle") {
				header.engine = Engine::ORACLE;
			} else if (value == "local") {
				header.engine = Engine::LOCAL;
			} else {
				Error(line, "@engine: expected oracle or local, not \"" + value + "\"");
				return;
			}
			engine_known = true;
		} else if (key == "version") {
			if (!ParseBoundedInteger(value, MAX_VERSION, number)) {
				Error(line, "@version: expected an integer from 1 to 2147483647, not \"" + value + "\"");
				return;
			}
			header.version = static_cast<int32_t>(number);
		} else if (key == "title") {
			if (Utf8Length(value) > MAX_TITLE_CHARACTERS) {
				Error(line, "@title is longer than " + std::to_string(MAX_TITLE_CHARACTERS) + " characters");
				return;
			}
			header.title = value;
		} else if (key == "description") {
			header.description += (description_lines++ ? "\n" : "") + value;
		} else if (key == "connection") {
			if (!IsIdentifier(value)) {
				Error(line,
				      "@connection: \"" + value + "\" is not a connection name: [a-z][a-z0-9_], up to 63 characters");
				return;
			}
			header.connection = value;
			connection_line = line;
		} else if (key == "param") {
			Param(line, value);
		} else if (key == "column") {
			Column(line, value);
		} else if (key == "depends") {
			Dependency dependency;
			std::string error;
			DependencyScanner scanner(value);
			if (!scanner.Parse(dependency, error)) {
				Error(line, error);
				return;
			}
			dependency.line = line;
			header.depends.push_back(std::move(dependency));
		} else if (key == "refresh") {
			if (value == "full") {
				return;
			}
			if (value.compare(0, 11, "incremental") == 0) {
				Error(line, "@refresh: incremental refresh is not part of the MVP; use full", CODE_UNSUPPORTED);
				return;
			}
			Error(line, "@refresh: expected full, not \"" + value + "\"");
		} else if (key == "max_rows") {
			if (!ParseBoundedInteger(value, MAX_MAX_ROWS, number)) {
				Error(line, "@max_rows: expected an integer from 1 to 1048575, not \"" + value + "\"");
				return;
			}
			header.max_rows = number;
		}
	}

	//! name ":" ptype [ "=" default ] [ "|" text ]
	void Param(int line, const std::string &value) {
		const auto colon = value.find(':');
		if (colon == std::string::npos) {
			Error(line, "@param is written as \"name: type [= default] [| description]\"");
			return;
		}
		QueryParam param;
		param.line = line;
		param.name = Trim(value.substr(0, colon));
		if (!IsIdentifier(param.name)) {
			Error(line, "@param: \"" + param.name +
			                "\" is not a valid name: [a-z][a-z0-9_], up to 63 characters, lowercase only");
			return;
		}
		const auto rest = value.substr(colon + 1);
		const auto type_end = rest.find_first_of("=|");
		std::string error;
		if (!ParseParamType(Trim(rest.substr(0, type_end)), param.type, error)) {
			Error(line, "@param " + param.name + ": " + error);
			return;
		}
		std::string default_text;
		std::string tail = type_end == std::string::npos ? std::string() : rest.substr(type_end);
		if (!tail.empty() && tail[0] == '=') {
			param.has_default = true;
			if (!DefaultText(tail.substr(1), default_text, tail, error)) {
				Error(line, "@param " + param.name + ": " + error);
				return;
			}
		}
		if (!tail.empty()) {
			// What is left starts with "|".
			param.description = Trim(tail.substr(1));
		}
		if (param.has_default && !CanonicalDefault(param, default_text, error)) {
			Error(line, "@param " + param.name + ": default value " + error);
			return;
		}
		for (const auto &existing : result.header.params) {
			if (Lower(existing.name) == Lower(param.name)) {
				Error(line,
				      "@param " + param.name + " is repeated (first on line " + std::to_string(existing.line) + ")");
				return;
			}
		}
		result.header.params.push_back(std::move(param));
	}

	//! `text` follows "=". Sets `value` and leaves in `tail` what follows the
	//! default: "" or text starting with "|".
	static bool DefaultText(const std::string &text, std::string &value, std::string &tail, std::string &error) {
		size_t at = 0;
		while (at < text.size() && IsSpace(text[at])) {
			at++;
		}
		if (at < text.size() && text[at] == '"') {
			at++;
			bool closed = false;
			while (at < text.size()) {
				if (text[at] == '\\' && at + 1 < text.size() && text[at + 1] == '"') {
					value += '"';
					at += 2;
				} else if (text[at] == '"') {
					at++;
					closed = true;
					break;
				} else {
					value += text[at++];
				}
			}
			if (!closed) {
				error = "unterminated quoted default value";
				return false;
			}
			const auto after = Trim(text.substr(at));
			if (!after.empty() && after[0] != '|') {
				error = "expected \"|\" or end of line after the quoted default value";
				return false;
			}
			tail = after;
			return true;
		}
		const auto bar = text.find('|');
		value = Trim(text.substr(0, bar));
		tail = bar == std::string::npos ? std::string() : text.substr(bar);
		if (value.empty()) {
			error = "no default value after \"=\"; an empty string is written as \"\"";
			return false;
		}
		return true;
	}

	//! A list default is its elements separated by commas, each trimmed; "" is
	//! the empty list.
	static bool CanonicalDefault(QueryParam &param, const std::string &text, std::string &error) {
		if (param.type.kind != ParamKind::LIST) {
			std::string canonical;
			if (!CanonicalizeText(param.type, text, canonical, error)) {
				return false;
			}
			param.default_value = CanonicalValue::Scalar(canonical);
			return true;
		}
		std::vector<std::string> elements;
		if (!Trim(text).empty()) {
			size_t start = 0;
			while (true) {
				const auto comma = text.find(',', start);
				const auto element = Trim(text.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
				std::string canonical;
				if (!CanonicalizeText(*param.type.element, element, canonical, error)) {
					return false;
				}
				elements.push_back(canonical);
				if (comma == std::string::npos) {
					break;
				}
				start = comma + 1;
			}
		}
		param.default_value = CanonicalValue::List(std::move(elements));
		return true;
	}

	//! colname ":" ctype [ "|" text ]
	void Column(int line, const std::string &value) {
		const auto colon = value.find(':');
		if (colon == std::string::npos) {
			Error(line, "@column is written as \"NAME: type [| header]\"");
			return;
		}
		QueryColumn column;
		column.line = line;
		column.name = Trim(value.substr(0, colon));
		if (!IsColumnName(column.name)) {
			Error(line, "@column: \"" + column.name + "\" is not a valid column name: [A-Za-z][A-Za-z0-9_$#]*");
			return;
		}
		const auto rest = value.substr(colon + 1);
		const auto bar = rest.find('|');
		std::string error;
		if (!ParseColumnType(Trim(rest.substr(0, bar)), column.type, error)) {
			Error(line, "@column " + column.name + ": " + error);
			return;
		}
		if (bar != std::string::npos) {
			column.header = Trim(rest.substr(bar + 1));
		}
		for (const auto &existing : result.header.columns) {
			if (Lower(existing.name) == Lower(column.name)) {
				Error(line, "@column " + column.name + " is repeated (first on line " + std::to_string(existing.line) +
				                "; names are compared case-insensitively)");
				return;
			}
		}
		result.header.columns.push_back(std::move(column));
	}

	//! What only the whole header can tell: required directives, rules that
	//! depend on @engine, references between directives.
	void CheckWholeHeader() {
		auto &header = result.header;
		for (const auto *required : {"id", "engine", "version"}) {
			if (first_line.find(required) == first_line.end()) {
				Error(0, std::string("missing required directive @") + required);
			}
		}
		if (engine_known && header.engine == Engine::LOCAL && connection_line) {
			Error(connection_line, "@connection is only allowed in a query with @engine: oracle");
		}
		for (const auto &dependency : header.depends) {
			if (engine_known && header.engine == Engine::ORACLE) {
				Error(dependency.line, "@depends is only allowed in a query with @engine: local");
				continue;
			}
			if (dependency.kind == Dependency::Kind::QUERY && !header.id.empty() && dependency.name == header.id) {
				Error(dependency.line, "@depends: a query cannot depend on itself");
			}
			for (const auto &argument : dependency.arguments) {
				if (argument.kind != DependencyArgument::Kind::PARAM) {
					continue;
				}
				bool declared = false;
				for (const auto &param : header.params) {
					declared = declared || param.name == argument.value;
				}
				if (!declared) {
					Error(dependency.line, "@depends: $" + argument.value + " is not declared in @param");
				}
			}
		}
		// Errors are reported in line order, the file-wide ones first.
		std::stable_sort(result.errors.begin(), result.errors.end(),
		                 [](const QueryParseError &left, const QueryParseError &right) { return left.line < right.line; });
	}

	ParsedQueryFile result;
	std::map<std::string, int> first_line;
	bool engine_known = false;
	int connection_line = 0;
	int description_lines = 0;
};

} // namespace

std::string QueryParseError::ToString() const {
	return line > 0 ? "line " + std::to_string(line) + ": " + message : message;
}

bool IsIdentifier(const std::string &text) {
	if (text.empty() || text.size() > 63 || text[0] < 'a' || text[0] > 'z') {
		return false;
	}
	for (const char c : text) {
		if (!((c >= 'a' && c <= 'z') || IsDigit(c) || c == '_')) {
			return false;
		}
	}
	return true;
}

ParsedQueryFile ParseQueryFile(const std::string &text) {
	return HeaderParser().Parse(text);
}

} // namespace ofexcel
