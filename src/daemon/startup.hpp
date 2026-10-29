#pragma once

// How ofexceld opens and configures its database before it serves
// (SPEC.md §8.4, TASKS.md A3). Part of the ofexceld binary, not of the
// extension: it loads fusion_scanner and core_functions explicitly. Internal to
// src/daemon and its tests; not an installed header.

#include "duckdb.hpp"

#include <string>

namespace ofexcel {

constexpr const char *DATABASE_FILE_NAME = "data.duckdb";
constexpr const char *INIT_FILE_NAME = "init.sql";

//! Applied when the database is opened, so init.sql can change them; SPEC.md §8.4.
constexpr const char *DEFAULT_MEMORY_LIMIT = "2GB";
constexpr int64_t DEFAULT_THREADS = 4;

struct StartupOptions {
	//! "" for QEXCEL_HOME/data.duckdb, ":memory:" for no file.
	std::string db_path;
	//! "" for QEXCEL_HOME/init.sql if that file exists. A file named here must exist.
	std::string init_file;
	//! Command-line flags; "" leaves the setting as init.sql left it.
	std::string port;
	std::string origin;
};

struct PreparedDatabase {
	duckdb::unique_ptr<duckdb::DuckDB> db;
	std::string db_path;
	//! "" if no init.sql ran.
	std::string init_file;
};

//! Opens the database with every extension ofexceld links. Nothing is
//! autoloaded or autoinstalled: what this binary can do is what is linked in.
duckdb::unique_ptr<duckdb::DuckDB> OpenDatabase(const std::string &path);

//! What `ofexceld serve` does before the server starts, in this order: check the
//! file name, open the database, run migrations, run init.sql, apply the
//! command-line flags, apply SPEC.md §8.4's settings, lock the configuration.
//! After it returns no connection can SET anything. Throws duckdb exceptions
//! whose message starts with a CONTRACTS.md §14 code.
PreparedDatabase PrepareDaemonDatabase(const StartupOptions &options);

//! Refuses a database file whose name, without the extension, is one of the
//! schemas the daemon creates: DuckDB names the catalog after the file, and
//! `meta.x` would then be ambiguous. Case-insensitive, as DuckDB's names are.
//! Throws duckdb::InvalidInputException (E_BAD_REQUEST).
void CheckDatabaseFileName(const std::string &path);

//! The whole file. Throws duckdb::IOException.
std::string ReadTextFile(const std::string &path);

} // namespace ofexcel
