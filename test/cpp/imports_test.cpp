// Importing a sheet's table over HTTPS (TASKS.md C7): begin, two batches,
// commit; xl_import.<name> replaced at once; a value that does not fit its
// column refused with nothing kept; an import left uncommitted for 10 minutes
// dropped, with the clock moved by the test. A live server started by serve's
// own start sequence; no Fusion.

#include "daemon/startup.hpp"
#include "ofexcel/imports.hpp"
#include "ofexcel/json_body.hpp"
#include "ofexcel/local_ca.hpp"
#include "ofexcel/paths.hpp"
#include "temporary_directory.hpp"
#include "test_support.hpp"

#include <httplib.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

using test_support::Contains;
using test_support::RunOk;

namespace fs = std::filesystem;

namespace {

const char *const ORIGIN = "https://app.example.com";
const char *const WORKBOOK = "0b6f3c1e-2a4d-4c8e-9f10-7d2e5a6b8c9d";

std::string ca_cert;

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

std::string SqlString(const std::string &text) {
	std::string quoted = "'";
	for (const auto c : text) {
		quoted += c;
		if (c == '\'') {
			quoted += '\'';
		}
	}
	return quoted + "'";
}

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

int PortOf(const std::string &url) {
	return std::stoi(url.substr(url.rfind(':') + 1));
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

	std::string Value(const std::string &sql) {
		const auto result = RunOk(*connection, sql);
		CHECK(result.rows.size() == 1);
		return result.At(0, 0).IsNull() ? "NULL" : result.At(0, 0).ToString();
	}
	std::string StageTables() {
		return Value("SELECT count(*) FROM duckdb_tables() WHERE schema_name = 'ds' AND "
		             "starts_with(table_name, 'stage__')");
	}
};

class Api {
public:
	explicit Api(int port) : client("127.0.0.1", port) {
		client.set_ca_cert_path(ca_cert.c_str());
		client.enable_server_certificate_verification(true);
		client.set_connection_timeout(5);
		client.set_read_timeout(30);
	}
	httplib::Result Get(const std::string &path) {
		return client.Get(path, Headers());
	}
	httplib::Result Post(const std::string &path, const std::string &body) {
		return client.Post(path, Headers(), body, "application/json");
	}

private:
	static httplib::Headers Headers() {
		return {{"Origin", ORIGIN}};
	}
	httplib::SSLClient client;
};

void CheckStatus(const httplib::Result &response, int status, const std::string &code = "") {
	CHECK(response);
	if (response->status != status || (!code.empty() && !Contains(response->body, "\"code\":\"" + code + "\""))) {
		std::cerr << "expected " << status << " " << code << ", got " << response->status << ": " << response->body
		          << "\n";
	}
	CHECK(response->status == status);
	if (!code.empty()) {
		CHECK(Contains(response->body, "\"code\":\"" + code + "\""));
	}
}

std::string Field(const std::string &body, const std::string &key) {
	const auto document = ofexcel::JsonDocument::Parse(body);
	const auto value = document.Root().Get(key);
	if (value.Is(ofexcel::JsonKind::NUMBER)) {
		return value.NumberText();
	}
	return value.Is(ofexcel::JsonKind::STRING) ? value.String() : "";
}

int64_t DataVersion(Api &api) {
	return std::stoll(api.Get("/v1/health")->get_header_value("X-OF-Data-Version"));
}

const std::string BANK_COLUMNS =
    "[{\"name\":\"Ref\",\"type\":\"string\"},{\"name\":\"Account\",\"type\":\"id\"},"
    "{\"name\":\"Amount\",\"type\":\"decimal(18,2)\"},{\"name\":\"Day\",\"type\":\"date\"},"
    "{\"name\":\"At\",\"type\":\"timestamp\"},{\"name\":\"N\",\"type\":\"integer\"},"
    "{\"name\":\"Rate\",\"type\":\"number\"},{\"name\":\"Cleared\",\"type\":\"boolean\"}]";

std::string BeginBody(const std::string &name, const std::string &columns) {
	return "{\"name\":\"" + name + "\",\"workbook_id\":\"" + WORKBOOK +
	       "\",\"source\":\"Sheet1!tblBank\",\"columns\":" + columns + "}";
}

std::string Begin(Api &api, const std::string &name, const std::string &columns) {
	const auto begun = api.Post("/v1/imports", BeginBody(name, columns));
	CheckStatus(begun, 201);
	const auto import_id = Field(begun->body, "import_id");
	CHECK(import_id.size() == 36);
	return import_id;
}

// --- Tests ------------------------------------------------------------------

//! Begin, two batches, commit: values typed as §3.2 stores them, in the order
//! sent, an empty cell NULL, leading zeros of an id kept.
void TestImport(Daemon &daemon) {
	Api api(daemon.port);
	const auto import_id = Begin(api, "bank", BANK_COLUMNS);
	CHECK(daemon.StageTables() == "1");
	CheckStatus(api.Post("/v1/imports/" + import_id + "/rows",
	                     "{\"rows\":[[\"INV-1\",\"0012\",100.25,\"2026-01-31\",\"2026-01-31T10:20:30.5\",7,0.1,true],"
	                     "[\"INV-2\",\"0013\",\"200\",\"2026-02-01\",null,8,1e3,\"N\"]]}"),
	            204);
	CheckStatus(api.Post("/v1/imports/" + import_id + "/rows", "{\"rows\":[[12,\"\",null,\"\",\"\",\"\",\"\",\"\"]]}"),
	            204);
	const auto before = DataVersion(api);
	const auto committed = api.Post("/v1/imports/" + import_id + "/commit", "{}");
	CheckStatus(committed, 200);
	CHECK(committed->body == "{\"name\":\"bank\",\"rows\":3}");
	CHECK(std::stoll(committed->get_header_value("X-OF-Data-Version")) == before + 1);
	CHECK(daemon.StageTables() == "0");

	const auto types = RunOk(*daemon.connection, "SELECT column_name, data_type FROM duckdb_columns() WHERE "
	                                             "schema_name = 'xl_import' AND table_name = 'bank' "
	                                             "ORDER BY column_index");
	const std::vector<std::pair<std::string, std::string>> expected = {
	    {"Ref", "VARCHAR"},   {"Account", "VARCHAR"}, {"Amount", "DECIMAL(18,2)"}, {"Day", "DATE"},
	    {"At", "TIMESTAMP"}, {"N", "BIGINT"},        {"Rate", "DOUBLE"},          {"Cleared", "BOOLEAN"}};
	CHECK(types.rows.size() == expected.size());
	for (size_t i = 0; i < expected.size(); i++) {
		CHECK(types.At(i, 0).ToString() == expected[i].first);
		CHECK(types.At(i, 1).ToString() == expected[i].second);
	}
	const auto rows = RunOk(*daemon.connection, "SELECT * FROM xl_import.bank ORDER BY rowid");
	CHECK(rows.rows.size() == 3);
	CHECK(rows.At(0, 0).ToString() == "INV-1");
	CHECK(rows.At(0, 1).ToString() == "0012");
	CHECK(rows.At(0, 2).ToString() == "100.25");
	CHECK(rows.At(0, 3).ToString() == "2026-01-31");
	CHECK(rows.At(0, 4).ToString() == "2026-01-31 10:20:30.5");
	CHECK(rows.At(0, 5).ToString() == "7");
	CHECK(rows.At(0, 6).ToString() == "0.1");
	CHECK(rows.At(0, 7).ToString() == "true");
	CHECK(rows.At(1, 2).ToString() == "200.00");
	CHECK(rows.At(1, 4).IsNull());
	CHECK(rows.At(1, 6).ToString() == "1000.0");
	CHECK(rows.At(1, 7).ToString() == "false");
	// A number in a string column, and empty cells.
	CHECK(rows.At(2, 0).ToString() == "12");
	for (size_t i = 1; i < expected.size(); i++) {
		CHECK(rows.At(2, i).IsNull());
	}

	CHECK(daemon.Value("SELECT workbook_id || ' ' || source || ' ' || row_count FROM meta.imports "
	                   "WHERE name = 'bank'") == std::string(WORKBOOK) + " Sheet1!tblBank 3");
	CHECK(daemon.Value("SELECT schema_json FROM meta.imports WHERE name = 'bank'") ==
	      "[{\"name\":\"Ref\",\"type\":\"string\"},{\"name\":\"Account\",\"type\":\"id\"},"
	      "{\"name\":\"Amount\",\"type\":\"decimal(18,2)\"},{\"name\":\"Day\",\"type\":\"date\"},"
	      "{\"name\":\"At\",\"type\":\"timestamp\"},{\"name\":\"N\",\"type\":\"integer\"},"
	      "{\"name\":\"Rate\",\"type\":\"number\"},{\"name\":\"Cleared\",\"type\":\"boolean\"}]");
	CHECK(daemon.Value("SELECT data_version FROM meta.imports WHERE name = 'bank'") ==
	      daemon.Value("SELECT value FROM meta.state WHERE key = 'data_version'"));

	// Committed once.
	CheckStatus(api.Post("/v1/imports/" + import_id + "/commit", "{}"), 404, "E_NOT_FOUND");
	CheckStatus(api.Post("/v1/imports/" + import_id + "/rows", "{\"rows\":[]}"), 404, "E_NOT_FOUND");
}

//! A new import of the same name replaces the table at commit, not before:
//! until then Local SQL reads the old rows.
void TestReplace(Daemon &daemon) {
	Api api(daemon.port);
	const auto import_id = Begin(api, "bank", "[{\"name\":\"ref\",\"type\":\"string\"},{\"name\":\"amount\","
	                                          "\"type\":\"decimal(18,2)\"}]");
	CheckStatus(api.Post("/v1/imports/" + import_id + "/rows", "{\"rows\":[[\"INV-9\",\"9.99\"]]}"), 204);
	CHECK(daemon.Value("SELECT count(*) FROM xl_import.bank") == "3");

	const auto local = "-- @id: banked\n-- @engine: local\n-- @version: 1\n-- @depends: xl_import.bank\n"
	                   "SELECT count(*) AS n FROM bank\n";
	auto preview = api.Post("/v1/preview", "{\"text\":" + JsonString(local) + "}");
	CheckStatus(preview, 200);
	CHECK(Contains(preview->body, "\"rows\":[[3]]"));

	CheckStatus(api.Post("/v1/imports/" + import_id + "/commit", "{}"), 200);
	CHECK(daemon.Value("SELECT count(*) FROM xl_import.bank") == "1");
	CHECK(daemon.Value("SELECT string_agg(column_name, ',' ORDER BY column_index) FROM duckdb_columns() WHERE "
	                   "schema_name = 'xl_import' AND table_name = 'bank'") == "ref,amount");
	CHECK(daemon.Value("SELECT count(*) FROM meta.imports") == "1");
	preview = api.Post("/v1/preview", "{\"text\":" + JsonString(local) + "}");
	CHECK(Contains(preview->body, "\"rows\":[[1]]"));
}

//! A value that does not fit its column is 422 naming row and column, and
//! the batch adds nothing; so does a row of the wrong width.
void TestRefused(Daemon &daemon) {
	Api api(daemon.port);
	const auto import_id = Begin(api, "checked", BANK_COLUMNS);
	CheckStatus(api.Post("/v1/imports/" + import_id + "/rows",
	                     "{\"rows\":[[\"a\",\"1\",1,\"2026-01-01\",null,1,1,true]]}"),
	            204);
	const auto refuse = [&](const std::string &row, const std::string &expected) {
		const auto refused = api.Post("/v1/imports/" + import_id + "/rows",
		                              "{\"rows\":[[\"b\",\"2\",2,\"2026-01-02\",null,2,2,false]," + row + "]}");
		CheckStatus(refused, 422, "E_BAD_REQUEST");
		if (!Contains(refused->body, expected)) {
			std::cerr << refused->body << "\n";
		}
		CHECK(Contains(refused->body, expected));
	};
	refuse("[\"c\",\"x12\",3,\"2026-01-03\",null,3,3,true]", "Row 3, column Account (id)");
	refuse("[\"c\",\"3\",1.234,\"2026-01-03\",null,3,3,true]", "Row 3, column Amount (decimal(18,2))");
	refuse("[\"c\",\"3\",3,\"2026-02-30\",null,3,3,true]", "Row 3, column Day (date)");
	refuse("[\"c\",\"3\",3,46023,null,3,3,true]", "Row 3, column Day (date)");
	refuse("[\"c\",\"3\",3,null,null,1.5,3,true]", "Row 3, column N (integer)");
	refuse("[\"c\",\"3\",3,null,null,\"12345678901234567890\",3,true]", "too large for type integer");
	refuse("[\"c\",\"3\",3,null,null,3,\"x\",true]", "Row 3, column Rate (number)");
	refuse("[\"c\",\"3\",3,null,null,3,3,\"maybe\"]", "Row 3, column Cleared (boolean)");
	refuse("[\"c\",\"3\",3,null,null,3,3,[true]]", "Row 3, column Cleared (boolean)");
	refuse("[\"c\",\"3\",3,null,null,3,3,{}]", "Row 2 of this batch, value 8");
	refuse("[\"c\",\"3\"]", "Row 3 has 2 values; the import has 8 columns");
	CheckStatus(api.Post("/v1/imports/" + import_id + "/rows", "{\"rows\":{}}"), 422, "E_BAD_REQUEST");
	CheckStatus(api.Post("/v1/imports/" + import_id + "/rows", "{\"rows\":[1]}"), 422, "E_BAD_REQUEST");
	CheckStatus(api.Post("/v1/imports/" + import_id + "/rows", "not json"), 400, "E_BAD_REQUEST");

	const auto committed = api.Post("/v1/imports/" + import_id + "/commit", "{}");
	CheckStatus(committed, 200);
	CHECK(committed->body == "{\"name\":\"checked\",\"rows\":1}");

	// What an import cannot begin with.
	const auto begin_refused = [&](const std::string &body) {
		CheckStatus(api.Post("/v1/imports", body), 422, "E_BAD_REQUEST");
	};
	begin_refused(BeginBody("Bank Statement", BANK_COLUMNS));
	begin_refused(BeginBody("1bank", BANK_COLUMNS));
	begin_refused(BeginBody("bank", "[]"));
	begin_refused(BeginBody("bank", "[{\"name\":\"t\",\"type\":\"timestamptz\"}]"));
	begin_refused(BeginBody("bank", "[{\"name\":\"t\",\"type\":\"money\"}]"));
	begin_refused(BeginBody("bank", "[{\"name\":\"a\",\"type\":\"string\"},{\"name\":\"A\",\"type\":\"id\"}]"));
	begin_refused(BeginBody("bank", "[{\"name\":\"\",\"type\":\"string\"}]"));
	begin_refused("{\"name\":\"bank\",\"workbook_id\":\"not-a-uuid\",\"source\":\"S!A1\",\"columns\":" + BANK_COLUMNS +
	              "}");
	begin_refused("{\"name\":\"bank\",\"workbook_id\":\"" + std::string(WORKBOOK) + "\",\"source\":\"\",\"columns\":" +
	              BANK_COLUMNS + "}");
	begin_refused("{\"name\":\"bank\",\"workbook_id\":\"" + std::string(WORKBOOK) + "\",\"source\":\"S!A1\"}");
	CheckStatus(api.Post("/v1/imports/nothing/rows", "{\"rows\":[]}"), 404, "E_NOT_FOUND");
	CHECK(daemon.StageTables() == "0");
}

//! An import with no batch or commit for 10 minutes is dropped, stage table
//! and all; a batch keeps it alive.
void TestExpiry(Daemon &daemon) {
	Api api(daemon.port);
	auto now = std::chrono::steady_clock::now();
	auto &store = ofexcel::ImportStore::Get();
	store.SetClock([&now]() { return now; });

	const auto kept = Begin(api, "kept", "[{\"name\":\"a\",\"type\":\"string\"}]");
	const auto dropped = Begin(api, "dropped", "[{\"name\":\"a\",\"type\":\"string\"}]");
	CHECK(daemon.StageTables() == "2");
	now += std::chrono::minutes(9);
	CheckStatus(api.Post("/v1/imports/" + kept + "/rows", "{\"rows\":[[\"x\"]]}"), 204);
	now += std::chrono::minutes(2);
	CheckStatus(api.Post("/v1/imports/" + dropped + "/rows", "{\"rows\":[[\"x\"]]}"), 404, "E_NOT_FOUND");
	CHECK(daemon.StageTables() == "1");
	CheckStatus(api.Post("/v1/imports/" + kept + "/commit", "{}"), 200);
	CHECK(daemon.Value("SELECT count(*) FROM xl_import.kept") == "1");
	CHECK(daemon.Value("SELECT count(*) FROM duckdb_tables() WHERE table_name = 'dropped'") == "0");
	CHECK(daemon.StageTables() == "0");

	store.SetClock([]() { return std::chrono::steady_clock::now(); });
}

} // namespace

int main() {
	test_support::TemporaryDirectory directory("ofexcel_imports_test");
	const auto home = ofexcel::JoinPath(directory.path, "home");
	SetQexcelHome(home);
	const auto tls = ofexcel::TlsFilesIn(ofexcel::DefaultTlsDir());
	ofexcel::InstallLocalCa(tls.dir);
	ca_cert = tls.ca_cert;
	// Never the user's own ~/QExcel/projects.
	WriteFile(ofexcel::JoinPath(home, ofexcel::INIT_FILE_NAME),
	          "SET ofexcel_projects_dir = " + SqlString(ofexcel::JoinPath(directory.path, "projects")) + ";\n");
	{
		Daemon daemon(ofexcel::JoinPath(directory.path, "data.duckdb"));
		TestImport(daemon);
		TestReplace(daemon);
		TestRefused(daemon);
		TestExpiry(daemon);
	}
	std::cout << "imports_test: all checks passed\n";
	return 0;
}
