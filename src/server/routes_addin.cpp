#include "routes.hpp"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

namespace ofexcel {

namespace {

namespace fs = std::filesystem;

// The files built into the binary until the real add-in is (TASKS.md E1): a
// placeholder task pane and functions.json from CONTRACTS.md §10.1.
const char *const BUILTIN_TASKPANE = R"QEXCEL(<!doctype html>
<html lang="ru">
<head>
<meta charset="utf-8">
<title>QExcel</title>
</head>
<body>
<p>QExcel: the add-in task pane is not built yet, so ofexceld serves a placeholder.</p>
</body>
</html>
)QEXCEL";

const char *const BUILTIN_FUNCTIONS = R"QEXCEL({
  "functions": [
    { "id": "TABLE", "name": "TABLE",
      "description": "QExcel query result as an array; the first row holds the headers",
      "parameters": [
        { "name": "query", "type": "string", "dimensionality": "scalar" },
        { "name": "args", "type": "any", "dimensionality": "matrix", "repeating": true, "optional": true } ],
      "result": { "type": "any", "dimensionality": "matrix" } },
    { "id": "CALL", "name": "CALL",
      "description": "Scalar value of a QExcel query (a 1 × 1 result)",
      "parameters": [
        { "name": "query", "type": "string", "dimensionality": "scalar" },
        { "name": "args", "type": "any", "dimensionality": "matrix", "repeating": true, "optional": true } ],
      "result": { "type": "any", "dimensionality": "scalar" } },
    { "id": "ASOF", "name": "ASOF",
      "description": "Time the query data was last fetched successfully",
      "parameters": [
        { "name": "query", "type": "string", "dimensionality": "scalar", "optional": true },
        { "name": "args", "type": "any", "dimensionality": "matrix", "repeating": true, "optional": true } ],
      "result": { "type": "number", "dimensionality": "scalar" } },
    { "id": "STATUS", "name": "STATUS",
      "description": "QExcel status: OK, NOT_SIGNED_IN, STALE, OFFLINE",
      "parameters": [],
      "result": { "type": "string", "dimensionality": "scalar" } }
  ]
}
)QEXCEL";

const std::map<std::string, std::string> &BuiltinFiles() {
	static const std::map<std::string, std::string> FILES = {{"taskpane.html", BUILTIN_TASKPANE},
	                                                         {"functions.json", BUILTIN_FUNCTIONS}};
	return FILES;
}

std::string Extension(const std::string &path) {
	const auto dot = path.rfind('.');
	const auto slash = path.rfind('/');
	if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) {
		return "";
	}
	std::string extension = path.substr(dot + 1);
	for (auto &c : extension) {
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	return extension;
}

bool IsHtml(const std::string &path) {
	const auto extension = Extension(path);
	return extension == "html" || extension == "htm";
}

const char *ContentType(const std::string &path) {
	static const std::map<std::string, const char *> TYPES = {
	    {"html", "text/html; charset=utf-8"},  {"htm", "text/html; charset=utf-8"},
	    {"js", "text/javascript; charset=utf-8"}, {"mjs", "text/javascript; charset=utf-8"},
	    {"json", "application/json; charset=utf-8"}, {"css", "text/css; charset=utf-8"},
	    {"map", "application/json; charset=utf-8"}, {"txt", "text/plain; charset=utf-8"},
	    {"xml", "application/xml; charset=utf-8"}, {"svg", "image/svg+xml"},
	    {"png", "image/png"}, {"ico", "image/x-icon"},
	    {"woff2", "font/woff2"}};
	const auto found = TYPES.find(Extension(path));
	return found == TYPES.end() ? "application/octet-stream" : found->second;
}

//! A path below /addin/ that cannot leave it: no "." or ".." segment, no empty
//! one, no backslash, drive letter or NUL. httplib has already decoded %2e.
bool IsSafeRelativePath(const std::string &relative) {
	if (relative.empty() || relative.find_first_of(std::string("\\:\0", 3)) != std::string::npos) {
		return false;
	}
	size_t start = 0;
	while (start <= relative.size()) {
		const auto end = relative.find('/', start);
		const auto segment = relative.substr(start, end == std::string::npos ? std::string::npos : end - start);
		if (segment.empty() || segment == "." || segment == "..") {
			return false;
		}
		if (end == std::string::npos) {
			break;
		}
		start = end + 1;
	}
	return true;
}

//! The file `relative` under `dir`, if it is a regular file that resolves
//! inside `dir` -- a symbolic link out of it does not count.
bool ReadFromDirectory(const std::string &dir, const std::string &relative, std::string &content) {
	std::error_code error;
	const auto base = fs::canonical(fs::u8path(dir), error);
	if (error) {
		return false;
	}
	const auto target = fs::weakly_canonical(base / fs::u8path(relative), error);
	if (error) {
		return false;
	}
	auto base_part = base.begin();
	auto target_part = target.begin();
	for (; base_part != base.end(); ++base_part, ++target_part) {
		if (target_part == target.end() || *base_part != *target_part) {
			return false;
		}
	}
	if (!fs::is_regular_file(target, error)) {
		return false;
	}
	std::ifstream file(target, std::ios::binary);
	if (!file) {
		return false;
	}
	std::ostringstream stream;
	stream << file.rdbuf();
	content = stream.str();
	return true;
}

//! `html` with <meta name="qexcel-session" content="…"> right after the
//! opening <head> tag, or at the very start if there is none. The token is hex,
//! so it needs no escaping.
std::string WithSessionToken(const std::string &html, const std::string &token) {
	const auto meta = "<meta name=\"qexcel-session\" content=\"" + token + "\">";
	std::string lower = html;
	for (auto &c : lower) {
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	for (auto at = lower.find("<head"); at != std::string::npos; at = lower.find("<head", at + 1)) {
		const char next = at + 5 < lower.size() ? lower[at + 5] : '\0';
		if (next != '>' && !std::isspace(static_cast<unsigned char>(next))) {
			continue; // <header>, not <head>
		}
		const auto close = lower.find('>', at);
		if (close == std::string::npos) {
			break;
		}
		return html.substr(0, close + 1) + meta + html.substr(close + 1);
	}
	return meta + html;
}

} // namespace

void RegisterAddinRoutes(httplib::Server &server, const RouteContext &context) {
	// GET /addin/<path> -- SPEC.md §5.1, local profile.
	server.Get(R"(/addin/(.*))", [context](const httplib::Request &request, httplib::Response &response) {
		const std::string relative = request.matches[1];
		std::string content;
		bool found = false;
		if (IsSafeRelativePath(relative)) {
			if (context.options.addin_dir.empty()) {
				const auto builtin = BuiltinFiles().find(relative);
				found = builtin != BuiltinFiles().end();
				if (found) {
					content = builtin->second;
				}
			} else {
				found = ReadFromDirectory(context.options.addin_dir, relative, content);
			}
		}
		if (!found) {
			SendError(response, 404, "E_NOT_FOUND", "No such add-in file");
			return;
		}
		if (IsHtml(relative)) {
			content = WithSessionToken(content, context.options.guard.session_token);
		}
		// The token changes with every start, so nothing here may be cached.
		response.set_header("Cache-Control", "no-store");
		response.set_header("X-Content-Type-Options", "nosniff");
		response.set_content(content, ContentType(relative));
	});
}

} // namespace ofexcel
