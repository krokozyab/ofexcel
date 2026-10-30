#include "ofexcel/oracle_render.hpp"

#include <algorithm>
#include <cctype>

namespace ofexcel {

namespace {

const char *const CODE_TOKENIZE = "E_SQL_TOKENIZE";
const char *const CODE_NOT_SELECT = "E_SQL_NOT_SELECT";
const char *const CODE_UNKNOWN = "E_PARAM_UNKNOWN";
const char *const CODE_POSITIONAL = "E_PARAM_POSITIONAL";
const char *const CODE_LIST_CONTEXT = "E_PARAM_LIST_CONTEXT";

bool IsLetter(char c) {
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

bool IsDigit(char c) {
	return c >= '0' && c <= '9';
}

//! What Oracle allows in an unquoted identifier or a number.
bool IsWordChar(char c) {
	return IsLetter(c) || IsDigit(c) || c == '_' || c == '$' || c == '#';
}

std::string Upper(std::string text) {
	for (auto &c : text) {
		c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
	}
	return text;
}

std::string Lower(std::string text) {
	for (auto &c : text) {
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	return text;
}

//! A :name in the text, in order.
struct Reference {
	std::string name; //!< lower case, as declared
	size_t output_at; //!< where in the copied text the literal goes
	bool after_in;
	int line;
};

//! Walks the body once: copies everything but :name references, which it
//! records, and reports the first structural error.
class Tokenizer {
public:
	explicit Tokenizer(const std::string &text) : text(text) {
	}

	bool Run(std::string &error_code, std::string &error_message) {
		if (!LeadingKeyword(error_code, error_message)) {
			return false;
		}
		while (at < text.size()) {
			const char c = text[at];
			if (c == '\'') {
				if (!Quoted('\'', "string literal")) {
					return Fail(CODE_TOKENIZE, "unterminated string literal", error_code, error_message);
				}
				Significant();
			} else if (c == '"') {
				if (!Quoted('"', "identifier")) {
					return Fail(CODE_TOKENIZE, "unterminated quoted identifier", error_code, error_message);
				}
				Significant();
			} else if (c == '-' && Next(1) == '-') {
				LineComment();
			} else if (c == '/' && Next(1) == '*') {
				if (!BlockComment()) {
					return Fail(CODE_TOKENIZE, "unterminated /* comment", error_code, error_message);
				}
			} else if (c == ':') {
				if (!Colon(error_code, error_message)) {
					return false;
				}
			} else if (IsWordChar(c)) {
				if (!Word(error_code, error_message)) {
					return false;
				}
			} else {
				if (c == '\n') {
					line++;
				}
				if (!std::isspace(static_cast<unsigned char>(c))) {
					Significant();
				}
				output += c;
				at++;
			}
		}
		return true;
	}

	std::string output;
	std::vector<Reference> references;

private:
	char Next(size_t offset) const {
		return at + offset < text.size() ? text[at + offset] : '\0';
	}

	bool Fail(const char *code, const std::string &message, std::string &error_code, std::string &error_message) const {
		error_code = code;
		error_message = "line " + std::to_string(line) + ": " + message;
		return false;
	}

	//! A token other than whitespace or a comment: the last word is no longer
	//! the one just before what comes next.
	void Significant() {
		last_word.clear();
	}

	//! §4.1: after whitespace and comments, SELECT or WITH.
	bool LeadingKeyword(std::string &error_code, std::string &error_message) {
		size_t i = 0;
		int lines = 1;
		while (i < text.size()) {
			if (std::isspace(static_cast<unsigned char>(text[i]))) {
				lines += text[i] == '\n';
				i++;
			} else if (text.compare(i, 2, "--") == 0) {
				while (i < text.size() && text[i] != '\n') {
					i++;
				}
			} else if (text.compare(i, 2, "/*") == 0) {
				const auto end = text.find("*/", i + 2);
				if (end == std::string::npos) {
					line = lines;
					return Fail(CODE_TOKENIZE, "unterminated /* comment", error_code, error_message);
				}
				for (size_t k = i; k < end; k++) {
					lines += text[k] == '\n';
				}
				i = end + 2;
			} else {
				break;
			}
		}
		size_t end = i;
		while (end < text.size() && IsWordChar(text[end])) {
			end++;
		}
		const auto keyword = Upper(text.substr(i, end - i));
		if (keyword != "SELECT" && keyword != "WITH") {
			line = lines;
			return Fail(CODE_NOT_SELECT, "the query must start with SELECT or WITH", error_code, error_message);
		}
		return true;
	}

	//! '…' or "…", the quote doubled inside; copied as is.
	bool Quoted(char quote, const char *) {
		output += text[at++];
		while (at < text.size()) {
			const char c = text[at];
			output += c;
			at++;
			if (c == '\n') {
				line++;
			}
			if (c == quote) {
				if (at < text.size() && text[at] == quote) {
					output += text[at++];
					continue;
				}
				return true;
			}
		}
		return false;
	}

	//! q'X…X' (and nq'…'): no escapes inside, the closing delimiter is X, or
	//! the matching bracket for [ ( { <.
	bool QLiteral(std::string &error_code, std::string &error_message) {
		// `at` is on the quote; the prefix is already copied.
		const char open = Next(1);
		if (open == '\0' || std::isspace(static_cast<unsigned char>(open))) {
			return Fail(CODE_TOKENIZE, "q-literal has no delimiter", error_code, error_message);
		}
		char close = open;
		switch (open) {
		case '[':
			close = ']';
			break;
		case '(':
			close = ')';
			break;
		case '{':
			close = '}';
			break;
		case '<':
			close = '>';
			break;
		default:
			break;
		}
		output += text[at++]; // '
		output += text[at++]; // open
		while (at < text.size()) {
			if (text[at] == close && Next(1) == '\'') {
				output += text[at++];
				output += text[at++];
				return true;
			}
			if (text[at] == '\n') {
				line++;
			}
			output += text[at++];
		}
		return Fail(CODE_TOKENIZE, "unterminated q-literal", error_code, error_message);
	}

	void LineComment() {
		while (at < text.size() && text[at] != '\n') {
			output += text[at++];
		}
	}

	bool BlockComment() {
		const auto end = text.find("*/", at + 2);
		if (end == std::string::npos) {
			return false;
		}
		for (size_t k = at; k < end + 2; k++) {
			line += text[k] == '\n';
			output += text[k];
		}
		at = end + 2;
		return true;
	}

	//! An unquoted word, or the prefix of a literal: N'…', q'…', nq'…'.
	bool Word(std::string &error_code, std::string &error_message) {
		const char first = text[at];
		const bool n_prefix = first == 'n' || first == 'N';
		const size_t q_at = n_prefix ? 1 : 0;
		const char q = Next(q_at);
		if ((q == 'q' || q == 'Q') && Next(q_at + 1) == '\'') {
			for (size_t k = 0; k <= q_at; k++) {
				output += text[at++];
			}
			if (!QLiteral(error_code, error_message)) {
				return false;
			}
			Significant();
			return true;
		}
		if (n_prefix && Next(1) == '\'') {
			output += text[at++];
			if (!Quoted('\'', "string literal")) {
				return Fail(CODE_TOKENIZE, "unterminated string literal", error_code, error_message);
			}
			Significant();
			return true;
		}
		const size_t start = at;
		while (at < text.size() && IsWordChar(text[at])) {
			output += text[at++];
		}
		last_word = Upper(text.substr(start, at - start));
		return true;
	}

	bool Colon(std::string &error_code, std::string &error_message) {
		const char next = Next(1);
		if (next == ':') {
			// "::" is copied whole: the second colon does not start a name.
			output += "::";
			at += 2;
			Significant();
			return true;
		}
		if (IsDigit(next)) {
			return Fail(CODE_POSITIONAL, "positional parameter :" + std::string(1, next) +
			                                 " is not supported; use :name from @param",
			            error_code, error_message);
		}
		if (next == '_' && Next(2) == '_') {
			size_t end = at + 1;
			while (end < text.size() && IsWordChar(text[end])) {
				end++;
			}
			return Fail(CODE_UNKNOWN, "parameter names starting with __ are reserved: " + text.substr(at, end - at),
			            error_code, error_message);
		}
		if (!IsLetter(next)) {
			output += text[at++];
			Significant();
			return true;
		}
		size_t end = at + 1;
		while (end < text.size() && (IsLetter(text[end]) || IsDigit(text[end]) || text[end] == '_')) {
			end++;
		}
		Reference reference;
		reference.name = Lower(text.substr(at + 1, end - at - 1));
		reference.output_at = output.size();
		reference.after_in = last_word == "IN";
		reference.line = line;
		references.push_back(std::move(reference));
		at = end;
		Significant();
		return true;
	}

	const std::string &text;
	size_t at = 0;
	int line = 1;
	//! The last significant token, upper case, if it was a word.
	std::string last_word;
};

std::string Quote(const std::string &value) {
	std::string quoted = "'";
	for (const char c : value) {
		quoted += c;
		if (c == '\'') {
			quoted += '\'';
		}
	}
	return quoted + "'";
}

std::string Numeric(const std::string &canonical) {
	return !canonical.empty() && canonical[0] == '-' ? "(" + canonical + ")" : canonical;
}

std::string ScalarLiteral(const ParamType &type, const std::string &canonical) {
	switch (type.kind) {
	case ParamKind::INTEGER:
	case ParamKind::ID:
	case ParamKind::NUMBER:
	case ParamKind::DECIMAL:
		return Numeric(canonical);
	case ParamKind::STRING:
	case ParamKind::ENUM:
		return Quote(canonical);
	case ParamKind::DATE:
		return "DATE " + Quote(canonical);
	case ParamKind::TIMESTAMP:
		return "TIMESTAMP " + Quote(canonical);
	case ParamKind::BOOLEAN:
		return canonical == "true" ? "'Y'" : "'N'";
	case ParamKind::LIST:
		break;
	}
	return "NULL";
}

const char *CollectionOf(const ParamType &element) {
	switch (element.kind) {
	case ParamKind::DATE:
	case ParamKind::TIMESTAMP:
		return "odcidatelist";
	case ParamKind::STRING:
	case ParamKind::BOOLEAN:
	case ParamKind::ENUM:
		return "odcivarchar2list";
	default:
		return "odcinumberlist";
	}
}

const QueryParam *Find(const std::vector<QueryParam> &params, const std::string &name) {
	for (const auto &param : params) {
		if (param.name == name) {
			return &param;
		}
	}
	return nullptr;
}

RenderResult Refused(const std::string &code, const std::string &message) {
	RenderResult result;
	result.code = code;
	result.message = message;
	return result;
}

//! The text's own errors, which do not depend on the values: the tokenizer's,
//! then every reference against @param. False with `code` and `message` set.
bool CheckText(Tokenizer &tokenizer, const std::vector<QueryParam> &params, std::string &code, std::string &message) {
	if (!tokenizer.Run(code, message)) {
		return false;
	}
	for (const auto &reference : tokenizer.references) {
		const auto *param = Find(params, reference.name);
		if (!param) {
			code = CODE_UNKNOWN;
			message = "line " + std::to_string(reference.line) + ": parameter :" + reference.name +
			          " is not declared in @param";
			return false;
		}
		const bool is_list = param->type.kind == ParamKind::LIST;
		if (is_list && !reference.after_in) {
			code = CODE_LIST_CONTEXT;
			message = "line " + std::to_string(reference.line) + ": list :" + reference.name +
			          " can only be substituted right after IN";
			return false;
		}
	}
	return true;
}

std::vector<std::string> UnusedParamWarnings(const Tokenizer &tokenizer, const std::vector<QueryParam> &params) {
	std::vector<std::string> warnings;
	for (const auto &param : params) {
		bool used = false;
		for (const auto &reference : tokenizer.references) {
			used = used || reference.name == param.name;
		}
		if (!used) {
			warnings.push_back("parameter " + param.name + " is declared but not used in the query text");
		}
	}
	return warnings;
}

} // namespace

OracleTextCheck CheckOracleText(const std::string &body, const std::vector<QueryParam> &params) {
	OracleTextCheck check;
	Tokenizer tokenizer(body);
	const bool text_ok = CheckText(tokenizer, params, check.code, check.message);
	// What the tokenizer found before it stopped is still what the text uses.
	for (const auto &reference : tokenizer.references) {
		if (std::find(check.params_in_text.begin(), check.params_in_text.end(), reference.name) ==
		    check.params_in_text.end()) {
			check.params_in_text.push_back(reference.name);
		}
	}
	if (!text_ok) {
		return check;
	}
	check.warnings = UnusedParamWarnings(tokenizer, params);
	check.ok = true;
	return check;
}

std::string OracleLiteral(const ParamType &type, const CanonicalValue &value) {
	if (type.kind != ParamKind::LIST) {
		return ScalarLiteral(type, value.scalar);
	}
	if (value.list.empty()) {
		return "(SELECT NULL FROM dual WHERE 1 = 0)";
	}
	const std::string collection = CollectionOf(*type.element);
	std::string subquery = "(";
	for (size_t start = 0; start < value.list.size(); start += ORACLE_LIST_CHUNK) {
		if (start) {
			subquery += " UNION ALL ";
		}
		subquery += "SELECT column_value FROM TABLE(sys." + collection + "(";
		const size_t end = std::min(value.list.size(), start + ORACLE_LIST_CHUNK);
		for (size_t i = start; i < end; i++) {
			subquery += (i > start ? ", " : "") + ScalarLiteral(*type.element, value.list[i]);
		}
		subquery += "))";
	}
	return subquery + ")";
}

RenderResult RenderOracle(const std::string &body, const std::vector<QueryParam> &params,
                          const std::map<std::string, ParamInput> &inputs) {
	// The text first: its errors do not depend on the values.
	Tokenizer tokenizer(body);
	std::string code;
	std::string message;
	if (!CheckText(tokenizer, params, code, message)) {
		return Refused(code, message);
	}

	// Then the values: every declared parameter, used or not, because all of
	// them make up canonical_params.
	auto normalized = NormalizeParams(params, inputs);
	if (!normalized.ok) {
		return Refused(normalized.code, normalized.message);
	}
	RenderResult result;
	result.values = std::move(normalized.values);

	// The literals, spliced in where the references were taken out.
	std::string rendered;
	size_t copied = 0;
	for (const auto &reference : tokenizer.references) {
		const auto *param = Find(params, reference.name);
		rendered += tokenizer.output.substr(copied, reference.output_at - copied);
		rendered += OracleLiteral(param->type, result.values[param->name]);
		copied = reference.output_at;
	}
	rendered += tokenizer.output.substr(copied);
	result.rendered = std::move(rendered);
	result.warnings = UnusedParamWarnings(tokenizer, params);
	result.ok = true;
	return result;
}

} // namespace ofexcel
