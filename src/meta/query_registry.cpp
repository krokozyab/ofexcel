#include "ofexcel/query_registry.hpp"

#include "ofexcel/api_error.hpp"
#include "ofexcel/db_util.hpp"
#include "ofexcel/hashing.hpp"
#include "ofexcel/utf8.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <utility>

namespace ofexcel {

namespace {

namespace fs = std::filesystem;

constexpr size_t MAX_PROJECT_NAME_BYTES = 100;
constexpr size_t CATALOG_VERSION_DIGITS = 16;

//! Every change is delete-then-insert of a whole source plus the new
//! catalog_version; two of them interleaved would leave a version that
//! describes neither. Process-wide, as the database is.
std::mutex &RegistryLock() {
	static std::mutex lock;
	return lock;
}

const char *EngineName(Engine engine) {
	return engine == Engine::LOCAL ? "local" : "oracle";
}

const char *ArgumentKindName(DependencyArgument::Kind kind) {
	switch (kind) {
	case DependencyArgument::Kind::PARAM:
		return "param";
	case DependencyArgument::Kind::NUMBER:
		return "number";
	case DependencyArgument::Kind::DATE:
		return "date";
	case DependencyArgument::Kind::BOOLEAN:
		return "boolean";
	default:
		return "string";
	}
}

CanonicalJson OptionalString(const std::string &value) {
	return value.empty() ? CanonicalJson::Null() : CanonicalJson::String(value);
}

//! A parsed file ready to become a row.
struct Parsed {
	QueryHeader header;
	std::string hash;
	std::string text;
};

//! The header's errors as "строка N: …; …", for a message on one line.
std::string JoinErrors(const std::vector<QueryParseError> &errors) {
	std::string joined;
	for (const auto &error : errors) {
		joined += (joined.empty() ? "" : "; ") + error.ToString();
	}
	return joined;
}

JsonObject ErrorsJson(const std::vector<QueryParseError> &errors) {
	JsonObject json;
	auto list = json.SetArray("errors");
	for (const auto &error : errors) {
		JsonObject item;
		item.Set("code", error.code).Set("message", error.message);
		if (error.line > 0) {
			item.Set("line", int64_t(error.line));
		}
		list.AddObject(item);
	}
	return json;
}

std::string DuplicateMessage(const std::string &id, int32_t version) {
	return "запрос " + id + " версии " + std::to_string(version) +
	       " уже есть в этом источнике с другим текстом: повысьте @version у одного из них";
}

void Insert(duckdb::Connection &connection, const char *kind, const std::string &ref, const Parsed &parsed) {
	// Two identical texts are one definition; the key makes that a no-op.
	QueryRows(connection,
	          "INSERT INTO meta.query_definitions VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9) ON CONFLICT DO NOTHING",
	          {duckdb::Value(parsed.hash), duckdb::Value(parsed.header.id), duckdb::Value::INTEGER(parsed.header.version),
	           duckdb::Value(EngineName(parsed.header.engine)), duckdb::Value(kind), duckdb::Value(ref),
	           duckdb::Value(HeaderJson(parsed.header)), duckdb::Value(parsed.text),
	           duckdb::Value::TIMESTAMP(duckdb::Timestamp::GetCurrentTimestamp())});
}

void UpdateCatalogVersion(duckdb::Connection &connection) {
	QueryRows(connection, "UPDATE meta.state SET value = $1 WHERE key = 'catalog_version'",
	          {duckdb::Value(ComputeCatalogVersion(connection))});
}

//! Replaces one source's definitions with `parsed` and recomputes the
//! catalog version. `ref` "" replaces every source of the kind.
void ReplaceSource(duckdb::Connection &connection, const char *kind, const std::string &ref,
                   const std::vector<std::pair<std::string, Parsed>> &parsed) {
	InTransaction(connection, [&]() {
		if (ref.empty()) {
			QueryRows(connection, "DELETE FROM meta.query_definitions WHERE source_kind = $1", {duckdb::Value(kind)});
		} else {
			QueryRows(connection, "DELETE FROM meta.query_definitions WHERE source_kind = $1 AND source_ref = $2",
			          {duckdb::Value(kind), duckdb::Value(ref)});
		}
		for (const auto &entry : parsed) {
			Insert(connection, kind, entry.first, entry.second);
		}
		UpdateCatalogVersion(connection);
	});
}

const char *const DEFINITION_COLUMNS =
    "SELECT d.definition_hash, d.query_id, d.version, d.engine, d.source_kind, d.source_ref, d.text, "
    "EXISTS (SELECT 1 FROM meta.trusted_definitions t WHERE t.definition_hash = d.definition_hash) "
    "FROM meta.query_definitions d ";

//! Workbook, then projects, then examples (§10.2); newest version first; ties
//! by source and hash, so no choice depends on row order.
const char *const DEFINITION_ORDER =
    " ORDER BY CASE d.source_kind WHEN 'workbook' THEN 0 WHEN 'project' THEN 1 ELSE 2 END, d.query_id, "
    "d.version DESC, d.source_ref, d.definition_hash";

QueryDefinition ToDefinition(const Row &row) {
	QueryDefinition definition;
	definition.definition_hash = row[0].ToString();
	definition.query_id = row[1].ToString();
	definition.version = row[2].GetValue<int32_t>();
	definition.engine = row[3].ToString();
	definition.source_kind = row[4].ToString();
	definition.source_ref = row[5].ToString();
	definition.text = row[6].ToString();
	definition.trusted = row[7].GetValue<bool>();
	return definition;
}

std::vector<QueryDefinition> ToDefinitions(const std::vector<Row> &rows) {
	std::vector<QueryDefinition> definitions;
	for (const auto &row : rows) {
		definitions.push_back(ToDefinition(row));
	}
	return definitions;
}

bool ReadFile(const fs::path &path, std::string &text, std::string &error) {
	std::error_code size_error;
	const auto size = fs::file_size(path, size_error);
	if (size_error) {
		error = "файл не прочитать: " + size_error.message();
		return false;
	}
	if (size > MAX_QUERY_FILE_BYTES) {
		error = "файл больше " + std::to_string(MAX_QUERY_FILE_BYTES / 1024) + " КБ";
		return false;
	}
	std::ifstream file(path, std::ios::binary);
	std::ostringstream content;
	content << file.rdbuf();
	if (!file) {
		error = "файл не прочитать";
		return false;
	}
	text = content.str();
	return true;
}

//! Entries sorted by their UTF-8 names, so a reload reports in a stable order.
std::vector<fs::path> SortedEntries(const fs::path &directory, bool directories) {
	std::vector<fs::path> entries;
	std::error_code error;
	for (fs::directory_iterator it(directory, error), end; !error && it != end; it.increment(error)) {
		std::error_code type_error;
		const bool wanted = directories ? it->is_directory(type_error) : it->is_regular_file(type_error);
		if (wanted && !type_error) {
			entries.push_back(it->path());
		}
	}
	std::sort(entries.begin(), entries.end(),
	          [](const fs::path &left, const fs::path &right) { return left.u8string() < right.u8string(); });
	return entries;
}

bool IsQueryFileName(const fs::path &path) {
	auto extension = path.extension().u8string();
	std::transform(extension.begin(), extension.end(), extension.begin(),
	               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return extension == ".sql";
}

//! One project's files: an entry for each, and the definitions that parsed.
void ScanProject(const fs::path &project_dir, const std::string &project, std::vector<ReloadEntry> &entries,
                 std::vector<std::pair<std::string, Parsed>> &parsed) {
	// (id, version) -> hash: one version of a query has one text.
	std::map<std::pair<std::string, int32_t>, std::string> seen;
	for (const auto &file : SortedEntries(project_dir / "queries", false)) {
		if (!IsQueryFileName(file)) {
			continue;
		}
		ReloadEntry entry;
		entry.source = project + "/queries/" + file.filename().u8string();
		std::string text;
		if (!ReadFile(file, text, entry.message)) {
			entries.push_back(entry);
			continue;
		}
		auto result = ParseQueryFile(text);
		entry.query_id = IsIdentifier(result.header.id) ? result.header.id : "";
		if (!result.Ok()) {
			entry.message = JoinErrors(result.errors);
			entries.push_back(entry);
			continue;
		}
		const auto hash = DefinitionHash(text);
		const auto key = std::make_pair(result.header.id, result.header.version);
		const auto previous = seen.find(key);
		if (previous != seen.end() && previous->second != hash) {
			entry.message = DuplicateMessage(result.header.id, result.header.version);
			entries.push_back(entry);
			continue;
		}
		seen[key] = hash;
		entry.ok = true;
		entries.push_back(entry);
		parsed.emplace_back(project, Parsed {std::move(result.header), hash, std::move(text)});
	}
}

std::string ProjectDirName(const fs::path &path) {
	return path.filename().u8string();
}

ApiError ParseError(const std::string &message, JsonObject details) {
	return ApiError("E_QUERY_PARSE", 422, message, std::move(details));
}

//! Parses a set given as one source -- a workbook's queries, the examples.
//! Returns the parsed texts in order, and the per-text errors (empty if all
//! parsed) with the duplicate rule applied.
std::vector<Parsed> ParseSet(const std::vector<std::string> &texts, std::vector<std::vector<QueryParseError>> &errors) {
	std::vector<Parsed> parsed;
	std::map<std::pair<std::string, int32_t>, std::string> seen;
	errors.assign(texts.size(), {});
	for (size_t i = 0; i < texts.size(); i++) {
		auto result = ParseQueryFile(texts[i]);
		Parsed item {std::move(result.header), DefinitionHash(texts[i]), texts[i]};
		if (result.Ok()) {
			const auto key = std::make_pair(item.header.id, item.header.version);
			const auto previous = seen.find(key);
			if (previous != seen.end() && previous->second != item.hash) {
				QueryParseError duplicate;
				duplicate.code = "E_QUERY_PARSE";
				duplicate.message = DuplicateMessage(item.header.id, item.header.version);
				result.errors.push_back(duplicate);
			}
			seen[key] = item.hash;
		}
		errors[i] = std::move(result.errors);
		parsed.push_back(std::move(item));
	}
	return parsed;
}

bool AnyErrors(const std::vector<std::vector<QueryParseError>> &errors) {
	return std::any_of(errors.begin(), errors.end(), [](const std::vector<QueryParseError> &e) { return !e.empty(); });
}

bool IsUuidV4(const std::string &text) {
	static const char *const PATTERN = "xxxxxxxx-xxxx-4xxx-Yxxx-xxxxxxxxxxxx";
	if (text.size() != 36) {
		return false;
	}
	for (size_t i = 0; i < text.size(); i++) {
		const char c = text[i];
		const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
		switch (PATTERN[i]) {
		case '-':
		case '4':
			if (c != PATTERN[i]) {
				return false;
			}
			break;
		case 'Y':
			if (c != '8' && c != '9' && c != 'a' && c != 'b') {
				return false;
			}
			break;
		default:
			if (!hex) {
				return false;
			}
		}
	}
	return true;
}

QueryDefinition DefinitionOf(duckdb::Connection &connection, const char *kind, const std::string &ref,
                             const std::string &hash) {
	const auto rows = QueryRows(connection,
	                            std::string(DEFINITION_COLUMNS) +
	                                "WHERE d.source_kind = $1 AND d.source_ref = $2 AND d.definition_hash = $3",
	                            {duckdb::Value(kind), duckdb::Value(ref), duckdb::Value(hash)});
	if (rows.empty()) {
		throw std::runtime_error("query definition " + hash + " vanished after it was registered");
	}
	return ToDefinition(rows[0]);
}

} // namespace

const std::vector<QueryText> &BuiltinExamples() {
	static const std::vector<QueryText> examples;
	return examples;
}

std::string HeaderJson(const QueryHeader &header) {
	auto params = CanonicalJson::Array();
	for (const auto &param : header.params) {
		auto default_value = CanonicalJson::Null();
		if (param.has_default && param.default_value.is_list) {
			default_value = CanonicalJson::Array();
			for (const auto &element : param.default_value.list) {
				default_value.Add(CanonicalJson::String(element));
			}
		} else if (param.has_default) {
			default_value = CanonicalJson::String(param.default_value.scalar);
		}
		params.Add(CanonicalJson::Object()
		               .Set("name", CanonicalJson::String(param.name))
		               .Set("type", CanonicalJson::String(param.type.ToString()))
		               .Set("default", std::move(default_value))
		               .Set("description", OptionalString(param.description)));
	}
	auto columns = CanonicalJson::Array();
	for (const auto &column : header.columns) {
		columns.Add(CanonicalJson::Object()
		                .Set("name", CanonicalJson::String(column.name))
		                .Set("type", CanonicalJson::String(column.type.ToString()))
		                .Set("header", OptionalString(column.header)));
	}
	auto depends = CanonicalJson::Array();
	for (const auto &dependency : header.depends) {
		auto arguments = CanonicalJson::Array();
		for (const auto &argument : dependency.arguments) {
			arguments.Add(CanonicalJson::Object()
			                  .Set("name", CanonicalJson::String(argument.name))
			                  .Set("kind", CanonicalJson::String(ArgumentKindName(argument.kind)))
			                  .Set("value", CanonicalJson::String(argument.value)));
		}
		depends.Add(CanonicalJson::Object()
		                .Set("kind", CanonicalJson::String(dependency.kind == Dependency::Kind::IMPORT ? "import" : "query"))
		                .Set("name", CanonicalJson::String(dependency.name))
		                .Set("arguments", std::move(arguments)));
	}
	return CanonicalJson::Object()
	    .Set("id", CanonicalJson::String(header.id))
	    .Set("engine", CanonicalJson::String(EngineName(header.engine)))
	    .Set("version", CanonicalJson::Integer(header.version))
	    .Set("title", OptionalString(header.title))
	    .Set("description", OptionalString(header.description))
	    .Set("connection", OptionalString(header.connection))
	    .Set("params", std::move(params))
	    .Set("columns", std::move(columns))
	    .Set("depends", std::move(depends))
	    .Set("max_rows", header.max_rows ? CanonicalJson::Integer(header.max_rows) : CanonicalJson::Null())
	    .Serialize();
}

bool IsProjectName(const std::string &name) {
	if (name.empty() || name.size() > MAX_PROJECT_NAME_BYTES || !IsValidUtf8(name) || name.front() == '.' ||
	    name.back() == '.' || name.back() == ' ') {
		return false;
	}
	for (const auto c : name) {
		const auto byte = static_cast<unsigned char>(c);
		if (byte < 0x20 || byte == 0x7F || std::string("/\\:*?\"<>|").find(c) != std::string::npos) {
			return false;
		}
	}
	return true;
}

std::vector<ReloadEntry> ReloadProjects(duckdb::Connection &connection, const std::string &projects_dir) {
	std::lock_guard<std::mutex> guard(RegistryLock());
	std::vector<ReloadEntry> entries;
	std::vector<std::pair<std::string, Parsed>> parsed;
	for (const auto &project_dir : SortedEntries(fs::u8path(projects_dir), true)) {
		const auto project = ProjectDirName(project_dir);
		if (!IsProjectName(project)) {
			continue;
		}
		ScanProject(project_dir, project, entries, parsed);
	}
	ReplaceSource(connection, SOURCE_PROJECT, "", parsed);
	return entries;
}

QueryDefinition SaveProjectQuery(duckdb::Connection &connection, const std::string &projects_dir,
                                 const std::string &project, const std::string &query_id, const std::string &text) {
	if (!IsIdentifier(query_id)) {
		throw ApiError("E_BAD_REQUEST", 422, "Идентификатор запроса должен соответствовать ^[a-z][a-z0-9_]{0,62}$");
	}
	if (!IsProjectName(project)) {
		throw ApiError("E_BAD_REQUEST", 422,
		               "Имя проекта должно годиться в имя каталога: без / \\ : * ? \" < > |, не с точки в начале");
	}
	if (text.size() > MAX_QUERY_FILE_BYTES) {
		throw ApiError("E_BAD_REQUEST", 422, "Текст запроса больше " + std::to_string(MAX_QUERY_FILE_BYTES / 1024) + " КБ");
	}
	auto result = ParseQueryFile(text);
	if (!result.Ok()) {
		throw ParseError("Заголовок запроса не разбирается: " + result.errors[0].ToString(), ErrorsJson(result.errors));
	}
	if (result.header.id != query_id) {
		QueryParseError mismatch;
		mismatch.code = "E_QUERY_PARSE";
		mismatch.message = "@id в тексте (" + result.header.id + ") не совпадает с идентификатором в адресе (" +
		                   query_id + ")";
		throw ParseError(mismatch.message, ErrorsJson({mismatch}));
	}

	std::lock_guard<std::mutex> guard(RegistryLock());
	const auto project_dir = fs::u8path(projects_dir) / fs::u8path(project);
	const auto queries_dir = project_dir / "queries";
	const auto target = queries_dir / fs::u8path(query_id + ".sql");
	const auto hash = DefinitionHash(text);
	// Checked before anything is written: another file of the project may
	// hold this id and version with another text, and the file would then be
	// on disk but not registered.
	for (const auto &file : SortedEntries(queries_dir, false)) {
		std::string other;
		std::string unreadable;
		if (!IsQueryFileName(file) || file.filename() == target.filename() || !ReadFile(file, other, unreadable)) {
			continue;
		}
		const auto other_result = ParseQueryFile(other);
		if (other_result.Ok() && other_result.header.id == query_id &&
		    other_result.header.version == result.header.version && DefinitionHash(other) != hash) {
			throw ParseError(DuplicateMessage(query_id, result.header.version) + " (файл " +
			                     file.filename().u8string() + ")",
			                 JsonObject());
		}
	}
	std::error_code error;
	fs::create_directories(queries_dir, error);
	if (error) {
		throw ApiError("E_INTERNAL", 500, "Не создать каталог " + queries_dir.u8string() + ": " + error.message());
	}
	// A temporary file renamed over the old one: the editor's file is never
	// left half written.
	const auto temporary = queries_dir / fs::u8path("." + query_id + ".sql.tmp");
	{
		std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
		file << text;
		file.close();
		if (!file) {
			throw ApiError("E_INTERNAL", 500, "Не записать файл " + temporary.u8string());
		}
	}
	fs::rename(temporary, target, error);
	if (error) {
		fs::remove(temporary, error);
		throw ApiError("E_INTERNAL", 500, "Не записать файл " + target.u8string());
	}

	std::vector<ReloadEntry> entries;
	std::vector<std::pair<std::string, Parsed>> parsed;
	ScanProject(project_dir, project, entries, parsed);
	ReplaceSource(connection, SOURCE_PROJECT, project, parsed);
	return DefinitionOf(connection, SOURCE_PROJECT, project, hash);
}

std::vector<QueryDefinition> RegisterWorkbook(duckdb::Connection &connection, const std::string &workbook_id,
                                              const std::vector<std::string> &texts) {
	if (!IsUuidV4(workbook_id)) {
		throw ApiError("E_BAD_REQUEST", 422, "Идентификатор книги должен быть UUID v4 в нижнем регистре");
	}
	std::vector<std::vector<QueryParseError>> errors;
	const auto parsed = ParseSet(texts, errors);
	if (AnyErrors(errors)) {
		JsonObject details;
		auto queries = details.SetArray("queries");
		std::string first;
		for (size_t i = 0; i < errors.size(); i++) {
			if (errors[i].empty()) {
				continue;
			}
			auto item = ErrorsJson(errors[i]);
			item.Set("index", int64_t(i));
			queries.AddObject(item);
			if (first.empty()) {
				first = "запрос №" + std::to_string(i + 1) + ": " + errors[i][0].ToString();
			}
		}
		throw ParseError("Запросы книги не зарегистрированы: " + first, std::move(details));
	}

	std::lock_guard<std::mutex> guard(RegistryLock());
	std::vector<std::pair<std::string, Parsed>> rows;
	for (const auto &item : parsed) {
		rows.emplace_back(workbook_id, item);
	}
	ReplaceSource(connection, SOURCE_WORKBOOK, workbook_id, rows);
	std::vector<QueryDefinition> definitions;
	for (const auto &item : parsed) {
		definitions.push_back(DefinitionOf(connection, SOURCE_WORKBOOK, workbook_id, item.hash));
	}
	return definitions;
}

void RegisterExamples(duckdb::Connection &connection, const std::vector<QueryText> &examples) {
	std::vector<std::string> texts;
	for (const auto &example : examples) {
		texts.push_back(example.text);
	}
	std::vector<std::vector<QueryParseError>> errors;
	const auto parsed = ParseSet(texts, errors);
	for (size_t i = 0; i < errors.size(); i++) {
		if (!errors[i].empty()) {
			throw std::runtime_error("built-in example " + examples[i].name + " does not parse: " + JoinErrors(errors[i]));
		}
	}
	std::lock_guard<std::mutex> guard(RegistryLock());
	std::vector<std::pair<std::string, Parsed>> rows;
	for (const auto &item : parsed) {
		rows.emplace_back(EXAMPLES_REF, item);
	}
	ReplaceSource(connection, SOURCE_EXAMPLE, EXAMPLES_REF, rows);
}

std::vector<QueryDefinition> ListDefinitions(duckdb::Connection &connection, const std::string &workbook_id) {
	return ToDefinitions(QueryRows(connection,
	                               std::string(DEFINITION_COLUMNS) +
	                                   "WHERE (d.source_kind = 'workbook' AND d.source_ref = $1) "
	                                   "OR d.source_kind IN ('project', 'example')" +
	                                   DEFINITION_ORDER,
	                               {duckdb::Value(workbook_id)}));
}

bool FindQuery(duckdb::Connection &connection, const std::string &query_id, const QueryScope &scope,
               QueryDefinition &found) {
	std::vector<Row> rows;
	if (!scope.source_kind.empty()) {
		rows = QueryRows(connection,
		                 std::string(DEFINITION_COLUMNS) +
		                     "WHERE d.query_id = lower($1) AND d.source_kind = $2 AND ($3 = '' OR d.source_ref = $3)" +
		                     DEFINITION_ORDER + " LIMIT 1",
		                 {duckdb::Value(query_id), duckdb::Value(scope.source_kind), duckdb::Value(scope.source_ref)});
	} else {
		rows = QueryRows(connection,
		                 std::string(DEFINITION_COLUMNS) +
		                     "WHERE d.query_id = lower($1) AND ((d.source_kind = 'workbook' AND d.source_ref = $2) "
		                     "OR d.source_kind IN ('project', 'example'))" +
		                     DEFINITION_ORDER + " LIMIT 1",
		                 {duckdb::Value(query_id), duckdb::Value(scope.workbook_id)});
	}
	if (rows.empty()) {
		return false;
	}
	found = ToDefinition(rows[0]);
	return true;
}

bool FindDefinitionByHash(duckdb::Connection &connection, const std::string &definition_hash, QueryDefinition &found) {
	const auto rows =
	    QueryRows(connection, std::string(DEFINITION_COLUMNS) + "WHERE d.definition_hash = $1" + DEFINITION_ORDER + " LIMIT 1",
	              {duckdb::Value(definition_hash)});
	if (rows.empty()) {
		return false;
	}
	found = ToDefinition(rows[0]);
	return true;
}

std::vector<QueryDefinition> AllDefinitions(duckdb::Connection &connection) {
	return ToDefinitions(QueryRows(connection, std::string(DEFINITION_COLUMNS) + DEFINITION_ORDER));
}

std::string ComputeCatalogVersion(duckdb::Connection &connection) {
	const auto rows = QueryRows(connection, "SELECT source_kind, source_ref, query_id, definition_hash "
	                                        "FROM meta.query_definitions ORDER BY 1, 2, 3, 4");
	if (rows.empty()) {
		return "";
	}
	std::string listing;
	for (const auto &row : rows) {
		listing += row[0].ToString() + "\t" + row[1].ToString() + "\t" + row[2].ToString() + "\t" + row[3].ToString() +
		           "\n";
	}
	return Sha256Hex(listing).substr(0, CATALOG_VERSION_DIGITS);
}

} // namespace ofexcel
