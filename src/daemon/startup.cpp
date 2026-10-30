#include "startup.hpp"

#include "core_functions_extension.hpp"
#include "fusion_scanner_extension.hpp"
#include "ofexcel/connections.hpp"
#include "ofexcel/datasets.hpp"
#include "ofexcel/db_util.hpp"
#include "ofexcel/migrations.hpp"
#include "ofexcel/paths.hpp"
#include "ofexcel/query_registry.hpp"
#include "ofexcel_extension.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace ofexcel {

namespace {

namespace fs = std::filesystem;

//! The schemas migrations.cpp creates (CONTRACTS.md §6.2).
const char *const RESERVED_CATALOG_NAMES[] = {"meta", "ds", "xl_import"};

std::string Lower(std::string text) {
	std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::tolower(c); });
	return text;
}

//! Quotes a value for use as a SQL string literal.
std::string SqlString(const std::string &value) {
	std::string quoted = "'";
	for (const auto c : value) {
		quoted += c;
		if (c == '\'') {
			quoted += '\'';
		}
	}
	return quoted + "'";
}

bool FileExists(const std::string &path) {
	std::error_code error;
	return fs::is_regular_file(fs::u8path(path), error);
}

//! QEXCEL_HOME, created private to the user if it does not exist yet: it holds
//! the database and, under tls/, the CA's key.
std::string EnsureHome() {
	std::string home;
	try {
		home = QexcelHome();
	} catch (const std::exception &error) {
		throw duckdb::InvalidInputException("E_BAD_REQUEST: %s; pass --db", error.what());
	}
	std::error_code error;
	const bool created = fs::create_directories(fs::u8path(home), error);
	if (error) {
		throw duckdb::IOException("E_INTERNAL: cannot create folder %s: %s", home, error.message());
	}
#ifndef _WIN32
	if (created) {
		::chmod(home.c_str(), 0700);
	}
#else
	(void)created;
#endif
	return home;
}

int ParsePort(const std::string &text) {
	const bool digits = !text.empty() && text.size() <= 5 &&
	                    std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isdigit(c); });
	const int port = digits ? std::stoi(text) : -1;
	if (port < 0 || port > 65535) {
		throw duckdb::InvalidInputException("E_BAD_REQUEST: --port must be a number from 0 to 65535, not '%s'", text);
	}
	return port;
}

//! Registers the project files (TASKS.md C1). A file that does not parse is
//! reported on stderr and left out; it never stops the daemon from starting.
void LoadProjects(duckdb::Connection &connection) {
	auto projects_dir =
	    QueryRows(connection, "SELECT current_setting('ofexcel_projects_dir')::VARCHAR").at(0).at(0).ToString();
	if (projects_dir.empty()) {
		try {
			projects_dir = DefaultProjectsDir();
		} catch (const std::exception &error) {
			std::cerr << "ofexceld: projects not loaded: " << error.what() << std::endl;
			return;
		}
	}
	size_t registered = 0;
	for (const auto &entry : ReloadProjects(connection, projects_dir)) {
		if (entry.ok) {
			registered++;
		} else {
			std::cerr << "ofexceld: " << entry.source << " not registered: " << entry.message << std::endl;
		}
	}
	std::cerr << "ofexceld: " << registered << " project queries from " << projects_dir << std::endl;
}

//! CONTRACTS.md §6.4: instances past ofexcel_dataset_ttl_days, stage tables and
//! runs a stopped daemon left behind.
void CleanUpAtStart(duckdb::Connection &connection) {
	const auto ttl_days =
	    QueryRows(connection, "SELECT current_setting('ofexcel_dataset_ttl_days')").at(0).at(0).GetValue<int64_t>();
	const auto cleanup = ofexcel::CleanupDatasets(connection, ttl_days > 0 ? ttl_days : DEFAULT_DATASET_TTL_DAYS);
	if (cleanup.expired_instances || cleanup.stage_tables || cleanup.interrupted_runs) {
		std::cerr << "ofexceld: dropped " << cleanup.expired_instances << " expired datasets and "
		          << cleanup.stage_tables << " stage tables; " << cleanup.interrupted_runs
		          << " interrupted runs marked failed" << std::endl;
	}
}

//! SPEC.md §8.4. Repeated even where OpenDatabase already set them, because
//! init.sql runs in between and may have changed them.
void Harden(duckdb::Connection &connection) {
	Execute(connection, "SET autoinstall_known_extensions = false");
	Execute(connection, "SET autoload_known_extensions = false");
	Execute(connection, "SET allow_community_extensions = false");
	Execute(connection, "SET enable_external_access = false");
	Execute(connection, "SET lock_configuration = true");
}

} // namespace

std::string ReadTextFile(const std::string &path) {
	std::ifstream file(fs::u8path(path), std::ios::binary);
	if (!file) {
		throw duckdb::IOException("E_BAD_REQUEST: cannot read file %s", path);
	}
	std::ostringstream content;
	content << file.rdbuf();
	return content.str();
}

duckdb::unique_ptr<duckdb::DuckDB> OpenDatabase(const std::string &path) {
	duckdb::DBConfig config;
	config.SetOptionByName("autoinstall_known_extensions", duckdb::Value::BOOLEAN(false));
	config.SetOptionByName("autoload_known_extensions", duckdb::Value::BOOLEAN(false));
	config.SetOptionByName("memory_limit", duckdb::Value(DEFAULT_MEMORY_LIMIT));
	config.SetOptionByName("threads", duckdb::Value::BIGINT(DEFAULT_THREADS));
	// Secrets stay in memory (SPEC.md §3.1): a persistent one is written in the
	// clear. Without this the secret manager also reads ~/.duckdb/stored_secrets
	// on every lookup by name, which enable_external_access = false refuses.
	config.SetOptionByName("allow_persistent_secrets", duckdb::Value::BOOLEAN(false));
	auto db = duckdb::make_uniq<duckdb::DuckDB>(path == ":memory:" ? nullptr : path.c_str(), &config);
	db->LoadStaticExtension<duckdb::CoreFunctionsExtension>();
	db->LoadStaticExtension<duckdb::FusionScannerExtension>();
	db->LoadStaticExtension<duckdb::OfexcelExtension>();
	return db;
}

void CheckDatabaseFileName(const std::string &path) {
	if (path == ":memory:") {
		return;
	}
	const auto stem = Lower(fs::u8path(path).stem().u8string());
	for (const auto *reserved : RESERVED_CATALOG_NAMES) {
		if (stem == reserved) {
			throw duckdb::InvalidInputException(
			    "E_BAD_REQUEST: database file %s has the same name as schema %s. DuckDB names the catalog after the "
			    "file, so references to %s.* would be ambiguous. Choose another name, for example %s",
			    path, reserved, reserved, DATABASE_FILE_NAME);
		}
	}
}

PreparedDatabase PrepareDaemonDatabase(const StartupOptions &options) {
	PreparedDatabase prepared;
	prepared.db_path = options.db_path;
	prepared.init_file = options.init_file;
	std::string home;
	if (prepared.db_path.empty()) {
		home = EnsureHome();
		prepared.db_path = JoinPath(home, DATABASE_FILE_NAME);
	} else if (prepared.init_file.empty()) {
		// An explicit --db needs no QEXCEL_HOME; only a default init.sql is looked for there.
		try {
			home = QexcelHome();
		} catch (const std::exception &) {
		}
	}
	if (prepared.init_file.empty() && !home.empty()) {
		const auto default_init = JoinPath(home, INIT_FILE_NAME);
		prepared.init_file = FileExists(default_init) ? default_init : "";
	}
	CheckDatabaseFileName(prepared.db_path);
	// Read before the database is opened, so a missing file does not leave a
	// fresh empty database behind.
	const auto init_sql = prepared.init_file.empty() ? std::string() : ReadTextFile(prepared.init_file);
	const int port = options.port.empty() ? -1 : ParsePort(options.port);

	prepared.db = OpenDatabase(prepared.db_path);
	duckdb::Connection connection(*prepared.db);
	// Migrations before init.sql: what init.sql calls may write to meta.*.
	RunMigrations(connection);
	if (!init_sql.empty()) {
		try {
			Execute(connection, init_sql);
		} catch (const std::exception &error) {
			throw duckdb::InvalidInputException("E_BAD_REQUEST: error in %s: %s", prepared.init_file,
			                                    duckdb::ErrorData(error).Message());
		}
	}
	// After init.sql, so a secret it created under a connection's name wins.
	RestoreConnectionSecrets(connection);
	// After init.sql too, which may set ofexcel_projects_dir.
	RegisterExamples(connection, BuiltinExamples());
	LoadProjects(connection);
	// After init.sql, which may set ofexcel_dataset_ttl_days.
	CleanUpAtStart(connection);
	if (port >= 0) {
		Execute(connection, "SET ofexcel_port = " + std::to_string(port));
	}
	if (!options.origin.empty()) {
		Execute(connection, "SET ofexcel_hosted_origin = " + SqlString(options.origin));
	}
	Harden(connection);
	return prepared;
}

} // namespace ofexcel
