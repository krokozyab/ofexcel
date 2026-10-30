// The start sequence of `ofexceld serve` (src/daemon/startup.cpp, TASKS.md A3),
// in process: the default database under QEXCEL_HOME, init.sql between the
// migrations and the lock, the command-line flags after init.sql, and a
// configuration nothing can change afterwards. Each test gets its own
// temporary QEXCEL_HOME.

#include "daemon/startup.hpp"
#include "ofexcel/paths.hpp"
#include "temporary_directory.hpp"
#include "test_support.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#ifndef _WIN32
#include <sys/stat.h>
#endif

using test_support::Contains;
using test_support::Run;
using test_support::RunOk;

namespace fs = std::filesystem;

namespace {

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

bool Exists(const std::string &path) {
	return fs::exists(fs::u8path(path));
}

//! The message PrepareDaemonDatabase refuses `options` with; aborts if it does not.
std::string Refusal(const ofexcel::StartupOptions &options) {
	try {
		ofexcel::PrepareDaemonDatabase(options);
	} catch (const std::exception &error) {
		return duckdb::ErrorData(error).Message();
	}
	CheckFailed("PrepareDaemonDatabase refused", __FILE__, __LINE__);
}

std::string Setting(duckdb::Connection &connection, const std::string &name) {
	return RunOk(connection, "SELECT current_setting('" + name + "')::VARCHAR").At(0, 0).ToString();
}

//! What current_setting('memory_limit') reads after SET memory_limit = `value`
//! on a database that is not locked: DuckDB reports it in its own units.
std::string MemoryLimitAsReported(const std::string &value) {
	test_support::TestDatabase db;
	duckdb::Connection connection(db);
	RunOk(connection, "SET memory_limit = '" + value + "'");
	return Setting(connection, "memory_limit");
}

void TestDefaultDatabaseLivesInQexcelHome() {
	test_support::TemporaryDirectory temporary("ofexcel_daemon_test");
	const auto home = ofexcel::JoinPath(temporary.path, "QExcel");
	SetQexcelHome(home);

	auto prepared = ofexcel::PrepareDaemonDatabase({});
	CHECK(prepared.db_path == ofexcel::JoinPath(home, ofexcel::DATABASE_FILE_NAME));
	CHECK(prepared.init_file.empty());
	CHECK(Exists(prepared.db_path));
#ifndef _WIN32
	struct stat info;
	CHECK(::stat(home.c_str(), &info) == 0 && (info.st_mode & 0777) == 0700);
#endif
	duckdb::Connection connection(*prepared.db);
	CHECK(RunOk(connection, "SELECT max(version) FROM meta.schema_version").At(0, 0).GetValue<int32_t>() == 1);
	// The catalog is named after the file, and is not one of the schemas.
	CHECK(RunOk(connection, "SELECT current_database()").At(0, 0).ToString() == "data");
}

void TestConfigurationIsLockedAfterStart() {
	test_support::TemporaryDirectory temporary("ofexcel_daemon_test");
	SetQexcelHome(temporary.path);
	auto prepared = ofexcel::PrepareDaemonDatabase({});
	duckdb::Connection connection(*prepared.db);

	// SPEC.md §8.4's values, read before anything tries to change them.
	CHECK(Setting(connection, "enable_external_access") == "false");
	CHECK(Setting(connection, "allow_community_extensions") == "false");
	CHECK(Setting(connection, "autoinstall_known_extensions") == "false");
	CHECK(Setting(connection, "autoload_known_extensions") == "false");
	CHECK(Setting(connection, "lock_configuration") == "true");
	CHECK(Setting(connection, "threads") == "4");
	CHECK(Setting(connection, "memory_limit") == MemoryLimitAsReported(ofexcel::DEFAULT_MEMORY_LIMIT));

	for (const auto *statement :
	     {"SET memory_limit = '8GB'", "SET threads = 1", "SET enable_external_access = true",
	      "SET lock_configuration = false", "SET ofexcel_port = 1", "SET ofexcel_hosted_origin = 'https://evil.example.com'",
	      "RESET enable_external_access"}) {
		const auto refused = Run(connection, statement);
		if (!refused.HasError()) {
			std::cerr << "not refused: " << statement << "\n";
		}
		CHECK(refused.HasError());
		CHECK(Contains(refused.error, "locked"));
	}
	// A fresh connection is no way around it.
	duckdb::Connection another(*prepared.db);
	CHECK(Run(another, "SET threads = 1").HasError());

	// External access is off: no reading files from SQL.
	const auto read = Run(connection, "SELECT * FROM read_csv('" + prepared.db_path + "')");
	CHECK(read.HasError());

	// Secrets live in memory only (SPEC.md §3.1), and a lookup by name never
	// reaches for ~/.duckdb, which external access being off would refuse.
	CHECK(Setting(connection, "allow_persistent_secrets") == "false");
	CHECK(Run(connection, "CREATE PERSISTENT SECRET kept (TYPE oracle_fusion, ENDPOINT 'https://fa.example.com', "
	                      "REPORT_PATH '/x.xdo')")
	          .HasError());
	RunOk(connection, "CREATE SECRET held (TYPE oracle_fusion, PROVIDER browser, ENDPOINT 'https://fa.example.com', "
	                  "REPORT_PATH '/x.xdo')");
	CHECK(RunOk(connection, "SELECT host FROM fusion_scanner_sso_status(secret := 'held')").At(0, 0).ToString() ==
	      "fa.example.com");
}

void TestInitSqlRunsBetweenMigrationsAndFlags() {
	test_support::TemporaryDirectory temporary("ofexcel_daemon_test");
	SetQexcelHome(temporary.path);
	const auto init = ofexcel::JoinPath(temporary.path, ofexcel::INIT_FILE_NAME);
	WriteFile(init, "CREATE TABLE main.seen AS SELECT max(version) AS v FROM meta.schema_version;\n"
	                "SET threads = 2;\n"
	                "SET ofexcel_port = 1111;\n"
	                "SET ofexcel_hosted_origin = 'https://from-init.example.com';\n"
	                "SET autoload_known_extensions = true;\n");

	ofexcel::StartupOptions options;
	options.port = "2222";
	auto prepared = ofexcel::PrepareDaemonDatabase(options);
	CHECK(prepared.init_file == init);
	duckdb::Connection connection(*prepared.db);

	// The migrations ran before init.sql.
	CHECK(RunOk(connection, "SELECT v FROM main.seen").At(0, 0).GetValue<int32_t>() == 1);
	// What init.sql chose stands unless a flag or §8.4 overrides it.
	CHECK(Setting(connection, "threads") == "2");
	CHECK(Setting(connection, "ofexcel_hosted_origin") == "https://from-init.example.com");
	// A flag wins over init.sql.
	CHECK(Setting(connection, "ofexcel_port") == "2222");
	// §8.4 wins over both. (enable_external_access needs no such test: DuckDB
	// refuses to turn it back on in a running database.)
	CHECK(Setting(connection, "autoload_known_extensions") == "false");
}

void TestOriginFlagWinsOverInitSql() {
	test_support::TemporaryDirectory temporary("ofexcel_daemon_test");
	SetQexcelHome(temporary.path);
	WriteFile(ofexcel::JoinPath(temporary.path, ofexcel::INIT_FILE_NAME),
	          "SET ofexcel_hosted_origin = 'https://from-init.example.com';\n");
	ofexcel::StartupOptions options;
	options.origin = "https://it's-quoted.example.com";
	auto prepared = ofexcel::PrepareDaemonDatabase(options);
	duckdb::Connection connection(*prepared.db);
	CHECK(Setting(connection, "ofexcel_hosted_origin") == options.origin);
}

void TestExplicitPathsAreUsed() {
	test_support::TemporaryDirectory temporary("ofexcel_daemon_test");
	SetQexcelHome(ofexcel::JoinPath(temporary.path, "home"));
	// A default init.sql that must not run when another one is named.
	WriteFile(ofexcel::JoinPath(temporary.path, "home/init.sql"), "CREATE TABLE main.wrong (x INTEGER);\n");
	const auto init = ofexcel::JoinPath(temporary.path, "custom.sql");
	WriteFile(init, "CREATE TABLE main.right (x INTEGER);\n");

	ofexcel::StartupOptions options;
	options.db_path = ofexcel::JoinPath(temporary.path, "work.duckdb");
	options.init_file = init;
	auto prepared = ofexcel::PrepareDaemonDatabase(options);
	CHECK(prepared.db_path == options.db_path);
	CHECK(Exists(options.db_path));
	duckdb::Connection connection(*prepared.db);
	CHECK(!Run(connection, "SELECT * FROM main.right").HasError());
	CHECK(Run(connection, "SELECT * FROM main.wrong").HasError());
}

void TestReopeningKeepsTheData() {
	test_support::TemporaryDirectory temporary("ofexcel_daemon_test");
	SetQexcelHome(temporary.path);
	{
		auto first = ofexcel::PrepareDaemonDatabase({});
		duckdb::Connection connection(*first.db);
		RunOk(connection, "CREATE TABLE ds.kept AS SELECT 42 AS answer");
	}
	auto second = ofexcel::PrepareDaemonDatabase({});
	duckdb::Connection connection(*second.db);
	CHECK(RunOk(connection, "SELECT answer FROM ds.kept").At(0, 0).GetValue<int32_t>() == 42);
	CHECK(RunOk(connection, "SELECT count(*) FROM meta.schema_version").At(0, 0).GetValue<int64_t>() == 1);
}

void TestReservedDatabaseNamesAreRefused() {
	test_support::TemporaryDirectory temporary("ofexcel_daemon_test");
	SetQexcelHome(temporary.path);
	// An explicit --db names a file in a directory that must already exist.
	fs::create_directories(fs::u8path(temporary.path));
	for (const auto *name : {"meta.duckdb", "META.duckdb", "ds.duckdb", "xl_import.db", "meta"}) {
		ofexcel::StartupOptions options;
		options.db_path = ofexcel::JoinPath(temporary.path, name);
		const auto message = Refusal(options);
		CHECK(Contains(message, "E_BAD_REQUEST"));
		CHECK(Contains(message, ofexcel::DATABASE_FILE_NAME));
		CHECK(!Exists(options.db_path));
	}
	// Names that only start like a schema are fine.
	ofexcel::StartupOptions options;
	options.db_path = ofexcel::JoinPath(temporary.path, "metadata.duckdb");
	ofexcel::PrepareDaemonDatabase(options);
}

void TestMissingOrBrokenInitIsRefused() {
	test_support::TemporaryDirectory temporary("ofexcel_daemon_test");
	SetQexcelHome(temporary.path);

	ofexcel::StartupOptions missing;
	missing.db_path = ofexcel::JoinPath(temporary.path, "a.duckdb");
	missing.init_file = ofexcel::JoinPath(temporary.path, "absent.sql");
	CHECK(Contains(Refusal(missing), "absent.sql"));
	// Refused before the database was opened: nothing left behind.
	CHECK(!Exists(missing.db_path));

	ofexcel::StartupOptions broken;
	broken.db_path = ofexcel::JoinPath(temporary.path, "b.duckdb");
	broken.init_file = ofexcel::JoinPath(temporary.path, "broken.sql");
	WriteFile(broken.init_file, "SELEC 1;\n");
	const auto message = Refusal(broken);
	CHECK(Contains(message, "E_BAD_REQUEST"));
	CHECK(Contains(message, "broken.sql"));

	ofexcel::StartupOptions bad_port;
	bad_port.db_path = ":memory:";
	for (const auto *port : {"abc", "-1", "65536", "80x"}) {
		bad_port.port = port;
		CHECK(Contains(Refusal(bad_port), "--port"));
	}
}

} // namespace

int main() {
	TestDefaultDatabaseLivesInQexcelHome();
	TestConfigurationIsLockedAfterStart();
	TestInitSqlRunsBetweenMigrationsAndFlags();
	TestOriginFlagWinsOverInitSql();
	TestExplicitPathsAreUsed();
	TestReopeningKeepsTheData();
	TestReservedDatabaseNamesAreRefused();
	TestMissingOrBrokenInitIsRefused();
	std::cout << "daemon_test: all checks passed\n";
	return 0;
}
