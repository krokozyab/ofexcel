// The query registry over HTTPS (TASKS.md C1): project files loaded by serve's
// start sequence and by ofexcel_reload(), a workbook's set replaced as a whole,
// saving a query from the editor, validation, resolving a name as §10.2 does,
// and catalog_version following the registry.

#include "daemon/startup.hpp"
#include "ofexcel/local_ca.hpp"
#include "ofexcel/paths.hpp"
#include "ofexcel/query_registry.hpp"
#include "temporary_directory.hpp"
#include "test_support.hpp"

#include <httplib.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

using test_support::Contains;
using test_support::Run;
using test_support::RunOk;

namespace fs = std::filesystem;

namespace {

const char *const ORIGIN = "https://app.example.com";
const char *const WORKBOOK = "0b6f3c1e-2a4d-4c8e-9f10-7d2e5a6b8c9d";
const char *const OTHER_WORKBOOK = "1c7a4d2f-3b5e-4d9f-8a21-8e3f6b7c9dae";

std::string ca_cert;
std::string projects_dir;

void SetQexcelHome(const std::string &path) {
#ifdef _WIN32
	CHECK(_putenv_s("QEXCEL_HOME", path.c_str()) == 0);
#else
	CHECK(setenv("QEXCEL_HOME", path.c_str(), 1) == 0);
#endif
}

void WriteFile(const std::string &path, const std::string &content) {
	fs::create_directories(fs::u8path(path).parent_path());
	std::ofstream file(fs::u8path(path), std::ios::binary);
	file << content;
	CHECK(file);
}

std::string ReadFile(const std::string &path) {
	std::ifstream file(fs::u8path(path), std::ios::binary);
	std::ostringstream content;
	content << file.rdbuf();
	return content.str();
}

std::string ProjectFile(const std::string &project, const std::string &file) {
	return ofexcel::JoinPath(ofexcel::JoinPath(ofexcel::JoinPath(projects_dir, project), "queries"), file);
}

int PortOf(const std::string &url) {
	return std::stoi(url.substr(url.rfind(':') + 1));
}

//! A query file. `extra` goes into the header, `body` after it.
std::string Query(const std::string &id, int version, const std::string &extra = "",
                  const std::string &body = "SELECT 1 FROM dual", const std::string &engine = "oracle") {
	return "-- @id: " + id + "\n-- @engine: " + engine + "\n-- @version: " + std::to_string(version) + "\n" + extra +
	       body + "\n";
}

//! A JSON string literal for `text`; enough for the texts these tests send.
std::string JsonString(const std::string &text) {
	std::string quoted = "\"";
	for (const auto c : text) {
		if (c == '"' || c == '\\') {
			quoted += '\\';
			quoted += c;
		} else if (c == '\n') {
			quoted += "\\n";
		} else {
			quoted += c;
		}
	}
	return quoted + "\"";
}

std::string WorkbookBody(const std::vector<std::string> &texts) {
	std::string body = "{\"queries\":[";
	for (size_t i = 0; i < texts.size(); i++) {
		body += (i ? ",{\"text\":" : "{\"text\":") + JsonString(texts[i]) + "}";
	}
	return body + "]}";
}

class Api {
public:
	explicit Api(int port) : client("127.0.0.1", port) {
		client.set_ca_cert_path(ca_cert.c_str());
		client.enable_server_certificate_verification(true);
		client.set_connection_timeout(5);
	}
	httplib::Result Get(const std::string &path) {
		return client.Get(path, httplib::Headers {{"Origin", ORIGIN}});
	}
	httplib::Result Post(const std::string &path, const std::string &body) {
		return client.Post(path, httplib::Headers {{"Origin", ORIGIN}}, body, "application/json");
	}
	httplib::Result Put(const std::string &path, const std::string &body) {
		return client.Put(path, httplib::Headers {{"Origin", ORIGIN}}, body, "application/json");
	}

private:
	httplib::SSLClient client;
};

void CheckStatus(const httplib::Result &response, int status, const std::string &code = "") {
	CHECK(response);
	if (response->status != status) {
		std::cerr << "expected " << status << ", got " << response->status << ": " << response->body << "\n";
	}
	CHECK(response->status == status);
	CHECK(response->has_header("X-OF-Catalog-Version"));
	if (!code.empty()) {
		CHECK(Contains(response->body, "\"code\":\"" + code + "\""));
	}
}

struct Daemon {
	ofexcel::PreparedDatabase prepared;
	std::unique_ptr<duckdb::Connection> connection;
	int port = 0;

	explicit Daemon(const std::string &db_path) {
		ofexcel::StartupOptions options;
		options.db_path = db_path;
		options.port = "0";
		options.origin = ORIGIN;
		prepared = ofexcel::PrepareDaemonDatabase(options);
		connection = std::make_unique<duckdb::Connection>(*prepared.db);
		port = PortOf(RunOk(*connection, "SELECT url FROM ofexcel_start()").At(0, 0).ToString());
	}
	~Daemon() {
		RunOk(*connection, "SELECT stopped FROM ofexcel_stop()");
	}

	std::string CatalogVersion() {
		return RunOk(*connection, "SELECT value FROM meta.state WHERE key = 'catalog_version'").At(0, 0).ToString();
	}
};

//! The projects on disk before the daemon starts.
void WriteProjects() {
	WriteFile(ProjectFile("fin", "gl_balance.sql"),
	          Query("gl_balance", 1, "-- @title: Остатки GL\n-- @param bu: integer = 204 | Бизнес-единица\n"
	                                 "-- @param ids: list<id> = 1, 2\n-- @column ACCOUNT: id | Счёт\n-- @column AMT: decimal(18,2)\n",
	                "SELECT account, amt FROM gl WHERE bu = :bu AND id IN :ids"));
	WriteFile(ProjectFile("fin", "gl_balance_v2.sql"), Query("gl_balance", 2));
	WriteFile(ProjectFile("fin", "broken.sql"), "-- @id: broken\n-- @engine: oracle\nSELECT 1 FROM dual\n");
	WriteFile(ProjectFile("fin", "notes.txt"), "not a query");
	WriteFile(ProjectFile("ap", "open_invoices.sql"), Query("ap_open", 1));
	// Another project with an older gl_balance: the newest version wins.
	WriteFile(ProjectFile("ap", "gl_balance.sql"), Query("gl_balance", 1, "-- @title: AP copy\n"));
	// One version, two texts, in one project: only the first is registered.
	WriteFile(ProjectFile("zz", "a.sql"), Query("twice", 1));
	WriteFile(ProjectFile("zz", "b.sql"), Query("twice", 1, "-- @title: other text\n"));
	WriteFile(ofexcel::JoinPath(ofexcel::JoinPath(projects_dir, ".hidden"), "queries/q.sql"), Query("hidden", 1));
}

void TestStartLoadsProjects(Daemon &daemon) {
	const auto rows = RunOk(*daemon.connection, "SELECT query_id, version, source_ref FROM ofexcel_queries() "
	                                            "WHERE source_kind = 'project' ORDER BY ALL");
	std::string listed;
	for (const auto &row : rows.rows) {
		listed += row[0].ToString() + "/" + row[1].ToString() + "/" + row[2].ToString() + " ";
	}
	CHECK(listed == "ap_open/1/ap gl_balance/1/ap gl_balance/1/fin gl_balance/2/fin twice/1/zz ");
}

void TestSummary(Daemon &daemon) {
	const auto response = Api(daemon.port).Get("/v1/queries/gl_balance?source_kind=project&source_ref=fin");
	CheckStatus(response, 200);
	// The newest version in that source.
	CHECK(Contains(response->body, "\"id\":\"gl_balance\",\"hash\":\""));
	CHECK(Contains(response->body, "\"version\":2,"));

	const auto v1 = ReadFile(ProjectFile("fin", "gl_balance.sql"));
	auto rows = RunOk(*daemon.connection, "SELECT definition_hash, header_json FROM meta.query_definitions "
	                                      "WHERE source_ref = 'fin' AND version = 1");
	CHECK(rows.At(0, 0).ToString() == ofexcel::DefinitionHash(v1));
	CHECK(rows.At(0, 1).ToString() ==
	      "{\"columns\":[{\"header\":\"Счёт\",\"name\":\"ACCOUNT\",\"type\":\"id\"},"
	      "{\"header\":null,\"name\":\"AMT\",\"type\":\"decimal(18,2)\"}],\"connection\":null,\"depends\":[],"
	      "\"description\":null,\"engine\":\"oracle\",\"id\":\"gl_balance\",\"max_rows\":null,"
	      "\"params\":[{\"default\":\"204\",\"description\":\"Бизнес-единица\",\"name\":\"bu\",\"type\":\"integer\"},"
	      "{\"default\":[\"1\",\"2\"],\"description\":null,\"name\":\"ids\",\"type\":\"list<id>\"}],"
	      "\"title\":\"Остатки GL\",\"version\":1}");

	// QuerySummary of version 1, read by its project.
	const auto hash = ofexcel::DefinitionHash(v1);
	const auto list = Api(daemon.port).Get("/v1/queries");
	CheckStatus(list, 200);
	CHECK(Contains(list->body,
	               "{\"id\":\"gl_balance\",\"hash\":\"" + hash +
	                   "\",\"version\":1,\"engine\":\"oracle\",\"title\":\"Остатки GL\","
	                   "\"params\":[{\"name\":\"bu\",\"type\":\"integer\",\"default\":\"204\",\"description\":\"Бизнес-"
	                   "единица\"},{\"name\":\"ids\",\"type\":\"list<id>\",\"default\":\"1, 2\"}],"
	                   "\"columns\":[{\"name\":\"ACCOUNT\",\"header\":\"Счёт\",\"type\":\"id\"},"
	                   "{\"name\":\"AMT\",\"header\":\"AMT\",\"type\":\"decimal(18,2)\"}],"
	                   "\"source\":{\"kind\":\"project\",\"ref\":\"fin\"},\"trusted\":false}"));
	// Projects come before examples; nothing from a workbook without ?workbook.
	CHECK(!Contains(list->body, "\"kind\":\"workbook\""));
}

void TestResolution(Daemon &daemon) {
	Api api(daemon.port);
	ofexcel::RegisterExamples(*daemon.connection,
	                          {{"gl", Query("gl_balance", 9, "-- @title: example\n")}, {"only", Query("ex_only", 1)}});

	// Projects before examples, even an example with a higher version.
	auto response = api.Get("/v1/queries/gl_balance");
	CheckStatus(response, 200);
	CHECK(Contains(response->body, "\"version\":2,"));
	CHECK(Contains(response->body, "\"source\":{\"kind\":\"project\",\"ref\":\"fin\"}"));
	CHECK(Contains(response->body, "\"text\":\"-- @id: gl_balance\\n"));
	// Without regard to case.
	response = api.Get("/v1/queries/GL_Balance");
	CheckStatus(response, 200);
	CHECK(Contains(response->body, "\"version\":2,"));
	// Only in the examples.
	response = api.Get("/v1/queries/ex_only");
	CheckStatus(response, 200);
	CHECK(Contains(response->body, "\"source\":{\"kind\":\"example\",\"ref\":\"builtin\"}"));
	response = api.Get("/v1/queries/gl_balance?source_kind=example");
	CheckStatus(response, 200);
	CHECK(Contains(response->body, "\"version\":9,"));

	// The workbook before everything, its newest version first.
	CheckStatus(api.Put(std::string("/v1/workbooks/") + WORKBOOK + "/queries",
	                    WorkbookBody({Query("gl_balance", 1, "-- @title: wb v1\n"),
	                                  Query("gl_balance", 3, "-- @title: wb v3\n")})),
	            200);
	response = api.Get(std::string("/v1/queries/gl_balance?workbook=") + WORKBOOK);
	CheckStatus(response, 200);
	CHECK(Contains(response->body, "\"title\":\"wb v3\""));
	CHECK(Contains(response->body, std::string("\"source\":{\"kind\":\"workbook\",\"ref\":\"") + WORKBOOK + "\"}"));
	// Another workbook does not see it.
	response = api.Get(std::string("/v1/queries/gl_balance?workbook=") + OTHER_WORKBOOK);
	CheckStatus(response, 200);
	CHECK(Contains(response->body, "\"source\":{\"kind\":\"project\",\"ref\":\"fin\"}"));

	CheckStatus(api.Get("/v1/queries/nope"), 404, "E_NOT_FOUND");
	CheckStatus(api.Get("/v1/queries/gl_balance?source_kind=elsewhere"), 422, "E_BAD_REQUEST");
}

void TestWorkbookSetIsReplaced(Daemon &daemon) {
	Api api(daemon.port);
	const auto path = std::string("/v1/workbooks/") + WORKBOOK + "/queries";
	CheckStatus(api.Put(path, WorkbookBody({})), 200);
	const auto without_workbook = daemon.CatalogVersion();
	CHECK(without_workbook.size() == 16);

	auto response = api.Put(path, WorkbookBody({Query("wb_a", 1), Query("wb_b", 1, "", "SELECT 1", "local")}));
	CheckStatus(response, 200);
	CHECK(Contains(response->body, "{\"queries\":[{\"id\":\"wb_a\","));
	CHECK(Contains(response->body, "\"engine\":\"local\""));
	CHECK(daemon.CatalogVersion() != without_workbook);
	CHECK(response->get_header_value("X-OF-Catalog-Version") == daemon.CatalogVersion());

	auto list = api.Get(std::string("/v1/queries?workbook=") + WORKBOOK);
	CHECK(Contains(list->body, "\"id\":\"wb_a\"") && Contains(list->body, "\"id\":\"wb_b\""));
	// The workbook's queries come first.
	CHECK(list->body.find("\"kind\":\"workbook\"") < list->body.find("\"kind\":\"project\""));

	// A second registration with another set: the old definitions go.
	CheckStatus(api.Put(path, WorkbookBody({Query("wb_c", 1)})), 200);
	list = api.Get(std::string("/v1/queries?workbook=") + WORKBOOK);
	CHECK(!Contains(list->body, "\"id\":\"wb_a\"") && !Contains(list->body, "\"id\":\"wb_b\""));
	CHECK(Contains(list->body, "\"id\":\"wb_c\""));
	CHECK(!Contains(list->body, "wb v3"));

	// One query that does not parse: nothing changes, and every error is named.
	response = api.Put(path, WorkbookBody({Query("wb_d", 1), "-- @id: Bad\nSELECT 1", Query("wb_e", 1)}));
	CheckStatus(response, 422, "E_QUERY_PARSE");
	CHECK(Contains(response->body, "\"details\":{\"queries\":[{\"errors\":["));
	CHECK(Contains(response->body, "\"index\":1}"));
	list = api.Get(std::string("/v1/queries?workbook=") + WORKBOOK);
	CHECK(Contains(list->body, "\"id\":\"wb_c\"") && !Contains(list->body, "\"id\":\"wb_d\""));

	// One version of a query has one text.
	CheckStatus(api.Put(path, WorkbookBody({Query("wb_f", 1), Query("wb_f", 1, "-- @title: x\n")})), 422,
	            "E_QUERY_PARSE");
	// The same text twice is one definition.
	response = api.Put(path, WorkbookBody({Query("wb_g", 1), Query("wb_g", 1)}));
	CheckStatus(response, 200);
	CHECK(RunOk(*daemon.connection,
	            std::string("SELECT count(*) FROM meta.query_definitions WHERE source_ref = '") + WORKBOOK + "'")
	          .At(0, 0) == duckdb::Value::BIGINT(1));

	CheckStatus(api.Put("/v1/workbooks/not-a-uuid/queries", WorkbookBody({})), 422, "E_BAD_REQUEST");
	CheckStatus(api.Put(path, "{\"queries\":{}}"), 422, "E_BAD_REQUEST");
	CheckStatus(api.Put(path, "{\"queries\":[{\"text\":1}]}"), 422, "E_BAD_REQUEST");
	CheckStatus(api.Put(path, "[]"), 400, "E_BAD_REQUEST");

	// An empty set removes the workbook's queries, and the version is a hash of
	// the registry: it comes back to what it was.
	CheckStatus(api.Put(path, WorkbookBody({})), 200);
	CHECK(daemon.CatalogVersion() == without_workbook);
	CHECK(ofexcel::ComputeCatalogVersion(*daemon.connection) == without_workbook);
}

void TestSaveFromEditor(Daemon &daemon) {
	Api api(daemon.port);
	const auto text = Query("new_query", 1, "-- @title: Новый\n");
	auto response = api.Put("/v1/queries/new_query", "{\"project\":\"fin\",\"text\":" + JsonString(text) + "}");
	CheckStatus(response, 200);
	CHECK(Contains(response->body, "\"id\":\"new_query\""));
	CHECK(Contains(response->body, "\"source\":{\"kind\":\"project\",\"ref\":\"fin\"}"));
	CHECK(ReadFile(ProjectFile("fin", "new_query.sql")) == text);
	CHECK(Contains(api.Get("/v1/queries/new_query")->body, "\"title\":\"Новый\""));

	// A new version replaces the file, and with it the old definition.
	const auto text2 = Query("new_query", 2, "-- @title: Новый 2\n");
	CheckStatus(api.Put("/v1/queries/new_query", "{\"project\":\"fin\",\"text\":" + JsonString(text2) + "}"), 200);
	CHECK(RunOk(*daemon.connection, "SELECT version FROM meta.query_definitions WHERE query_id = 'new_query'").rows.size() ==
	      1);
	// A project that does not exist yet is created.
	CheckStatus(api.Put("/v1/queries/fresh", "{\"project\":\"Новый проект\",\"text\":" + JsonString(Query("fresh", 1)) + "}"),
	            200);
	CHECK(fs::exists(fs::u8path(ProjectFile("Новый проект", "fresh.sql"))));

	// @id in the text must be the one in the path.
	response = api.Put("/v1/queries/other_id", "{\"project\":\"fin\",\"text\":" + JsonString(Query("new_query", 3)) + "}");
	CheckStatus(response, 422, "E_QUERY_PARSE");
	CHECK(!fs::exists(fs::u8path(ProjectFile("fin", "other_id.sql"))));
	response = api.Put("/v1/queries/bad", "{\"project\":\"fin\",\"text\":\"-- @id: bad\\nSELECT 1\"}");
	CheckStatus(response, 422, "E_QUERY_PARSE");
	CHECK(Contains(response->body, "\"details\":{\"errors\":[{\"code\":\"E_QUERY_PARSE\""));
	// Another file of the project, gl_balance_v2.sql, has this id and version with
	// another text. gl_balance.sql itself would simply be replaced.
	response = api.Put("/v1/queries/gl_balance", "{\"project\":\"fin\",\"text\":" +
	                                                  JsonString(Query("gl_balance", 2, "-- @title: clash\n")) + "}");
	CheckStatus(response, 422, "E_QUERY_PARSE");
	CHECK(!fs::exists(fs::u8path(ProjectFile("fin", "gl_balance.sql.tmp"))));
	CHECK(Contains(ReadFile(ProjectFile("fin", "gl_balance.sql")), "Остатки GL"));

	for (const auto *project : {"../escape", "a/b", ".hidden", "", "con:"}) {
		CheckStatus(api.Put("/v1/queries/x", std::string("{\"project\":") + JsonString(project) + ",\"text\":" +
		                                         JsonString(Query("x", 1)) + "}"),
		            422, "E_BAD_REQUEST");
	}
	CHECK(!fs::exists(fs::u8path(ofexcel::JoinPath(projects_dir, "../escape"))));
	CheckStatus(api.Put("/v1/queries/Bad_Id", "{\"project\":\"fin\",\"text\":\"x\"}"), 422, "E_BAD_REQUEST");
	CheckStatus(api.Put("/v1/queries/x", "{\"text\":\"x\"}"), 422, "E_BAD_REQUEST");
}

void TestValidate(Daemon &daemon) {
	Api api(daemon.port);
	// Oracle: the parameters used, and a warning for the one that is not.
	auto response = api.Post("/v1/queries/validate",
	                         "{\"text\":" +
	                             JsonString(Query("v", 1, "-- @param bu: integer\n-- @param unused: string = \"\"\n",
	                                              "SELECT * FROM t WHERE bu = :BU")) +
	                             "}");
	CheckStatus(response, 200);
	CHECK(Contains(response->body, "{\"ok\":true,\"header\":{\"id\":\"v\","));
	CHECK(Contains(response->body, "\"source\":{\"kind\":\"workbook\",\"ref\":\"\"},\"trusted\":false}"));
	CHECK(Contains(response->body, "\"params_in_text\":[\"bu\"],\"errors\":[],\"warnings\":[{\"code\":\"W_PARAM_UNUSED\""));

	// A registered text says where it is registered.
	response = api.Post("/v1/queries/validate", "{\"text\":" + JsonString(ReadFile(ProjectFile("ap", "open_invoices.sql"))) + "}");
	CheckStatus(response, 200);
	CHECK(Contains(response->body, "\"source\":{\"kind\":\"project\",\"ref\":\"ap\"}"));

	// The text's errors.
	response = api.Post("/v1/queries/validate", "{\"text\":" + JsonString(Query("v", 1, "", "SELECT :nope FROM dual")) + "}");
	CheckStatus(response, 200);
	CHECK(Contains(response->body, "{\"ok\":false,\"header\":"));
	CHECK(Contains(response->body, "\"params_in_text\":[\"nope\"],\"errors\":[{\"code\":\"E_PARAM_UNKNOWN\""));

	// The header's errors, with their lines; no header then.
	response = api.Post("/v1/queries/validate", "{\"text\":\"-- @id: v\\n-- @engine: oracle\\n-- @version: x\\nSELECT 1\"}");
	CheckStatus(response, 200);
	CHECK(Contains(response->body, "{\"ok\":false,\"params_in_text\":[],\"errors\":[{\"code\":\"E_QUERY_PARSE\""));
	CHECK(Contains(response->body, "\"line\":3}"));

	// Local: the validator's rules and $name.
	response = api.Post("/v1/queries/validate",
	                    "{\"text\":" +
	                        JsonString(Query("l", 1, "-- @param p: integer\n", "SELECT $p AS x", "local")) + "}");
	CheckStatus(response, 200);
	CHECK(Contains(response->body, "\"ok\":true"));
	CHECK(Contains(response->body, "\"params_in_text\":[\"p\"]"));
	response = api.Post("/v1/queries/validate",
	                    "{\"text\":" + JsonString(Query("l", 1, "", "SELECT * FROM read_csv('x.csv')", "local")) + "}");
	CheckStatus(response, 200);
	CHECK(Contains(response->body, "\"ok\":false"));
	CHECK(Contains(response->body, "\"code\":\"E_VALIDATION\""));

	CheckStatus(api.Post("/v1/queries/validate", "{}"), 422, "E_BAD_REQUEST");
}

void TestReload(Daemon &daemon) {
	WriteFile(ProjectFile("ap", "late.sql"), Query("late", 1));
	fs::remove(fs::u8path(ProjectFile("ap", "open_invoices.sql")));
	const auto rows = RunOk(*daemon.connection, "SELECT source, query_id, ok, message FROM ofexcel_reload()");
	std::string report;
	for (const auto &row : rows.rows) {
		report += row[0].ToString() + "|" + (row[1].IsNull() ? "NULL" : row[1].ToString()) + "|" + row[2].ToString() +
		          "|" + (row[3].IsNull() ? "" : "msg") + "\n";
	}
	CHECK(report == "ap/queries/gl_balance.sql|gl_balance|true|\n"
	                "ap/queries/late.sql|late|true|\n"
	                "fin/queries/broken.sql|broken|false|msg\n"
	                "fin/queries/gl_balance.sql|gl_balance|true|\n"
	                "fin/queries/gl_balance_v2.sql|gl_balance|true|\n"
	                "fin/queries/new_query.sql|new_query|true|\n"
	                "zz/queries/a.sql|twice|true|\n"
	                "zz/queries/b.sql|twice|false|msg\n"
	                "Новый проект/queries/fresh.sql|fresh|true|\n");
	CHECK(Contains(rows.At(2, 3).ToString(), "@version"));
	CheckStatus(Api(daemon.port).Get("/v1/queries/ap_open"), 404, "E_NOT_FOUND");
	CheckStatus(Api(daemon.port).Get("/v1/queries/late"), 200);
	// The examples and workbooks are not the project files' business.
	CheckStatus(Api(daemon.port).Get("/v1/queries/ex_only"), 200);
}

} // namespace

int main() {
	test_support::TemporaryDirectory directory("ofexcel_queries_test");
	const auto home = ofexcel::JoinPath(directory.path, "home");
	SetQexcelHome(home);
	const auto tls = ofexcel::TlsFilesIn(ofexcel::DefaultTlsDir());
	ofexcel::InstallLocalCa(tls.dir);
	ca_cert = tls.ca_cert;
	projects_dir = ofexcel::JoinPath(directory.path, "projects");
	WriteProjects();
	// Never the user's own ~/QExcel/projects.
	std::string quoted = "'";
	for (const auto c : projects_dir) {
		quoted += c;
		if (c == '\'') {
			quoted += '\'';
		}
	}
	WriteFile(ofexcel::JoinPath(home, ofexcel::INIT_FILE_NAME), "SET ofexcel_projects_dir = " + quoted + "';\n");

	Daemon daemon(ofexcel::JoinPath(directory.path, "data.duckdb"));
	TestStartLoadsProjects(daemon);
	TestSummary(daemon);
	TestResolution(daemon);
	TestWorkbookSetIsReplaced(daemon);
	TestSaveFromEditor(daemon);
	TestValidate(daemon);
	TestReload(daemon);

	std::cout << "queries_test: all checks passed\n";
	return 0;
}
