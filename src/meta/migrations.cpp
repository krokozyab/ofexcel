#include "ofexcel/migrations.hpp"

#include "ofexcel/db_util.hpp"

#include <vector>

namespace ofexcel {

namespace {

//! Version 1, exactly as CONTRACTS.md §6.2 states it. JSON columns are VARCHAR:
//! the json extension is not linked into this binary.
const std::vector<const char *> &SchemaV1() {
	static const std::vector<const char *> statements = {
	    "CREATE SCHEMA IF NOT EXISTS ds",
	    "CREATE SCHEMA IF NOT EXISTS xl_import",
	    R"(CREATE TABLE meta.state (
	        key   VARCHAR PRIMARY KEY,
	        value VARCHAR NOT NULL))",
	    R"(CREATE TABLE meta.connections (
	        name        VARCHAR PRIMARY KEY,
	        environment VARCHAR NOT NULL CHECK (environment IN ('DEV', 'TEST', 'PROD')),
	        endpoint    VARCHAR NOT NULL,
	        report_path VARCHAR NOT NULL,
	        created_at  TIMESTAMP NOT NULL))",
	    R"(CREATE TABLE meta.query_definitions (
	        definition_hash VARCHAR NOT NULL,
	        query_id        VARCHAR NOT NULL,
	        version         INTEGER NOT NULL,
	        engine          VARCHAR NOT NULL CHECK (engine IN ('oracle', 'local')),
	        source_kind     VARCHAR NOT NULL CHECK (source_kind IN ('project', 'workbook', 'example')),
	        source_ref      VARCHAR NOT NULL,
	        header_json     VARCHAR NOT NULL,
	        text            VARCHAR NOT NULL,
	        registered_at   TIMESTAMP NOT NULL,
	        PRIMARY KEY (source_kind, source_ref, query_id, definition_hash)))",
	    R"(CREATE TABLE meta.query_schemas (
	        definition_hash VARCHAR NOT NULL,
	        ordinal         INTEGER NOT NULL,
	        column_name     VARCHAR NOT NULL,
	        contract_type   VARCHAR NOT NULL,
	        header          VARCHAR,
	        PRIMARY KEY (definition_hash, ordinal)))",
	    R"(CREATE TABLE meta.dataset_instances (
	        instance_key    VARCHAR PRIMARY KEY,
	        query_id        VARCHAR NOT NULL,
	        definition_hash VARCHAR NOT NULL,
	        connection_name VARCHAR NOT NULL,
	        params_json     VARCHAR NOT NULL,
	        current_table   VARCHAR,
	        current_version INTEGER NOT NULL DEFAULT 0,
	        prev_table      VARCHAR,
	        data_version    BIGINT  NOT NULL DEFAULT 0,
	        fetched_at      TIMESTAMP,
	        row_count       BIGINT,
	        truncated       BOOLEAN,
	        status          VARCHAR NOT NULL CHECK (status IN ('empty', 'ready')),
	        last_error_code VARCHAR,
	        last_error      VARCHAR,
	        last_access_at  TIMESTAMP NOT NULL))",
	    R"(CREATE TABLE meta.runs (
	        run_id          VARCHAR PRIMARY KEY,
	        instance_key    VARCHAR NOT NULL,
	        query_id        VARCHAR NOT NULL,
	        definition_hash VARCHAR NOT NULL,
	        connection_name VARCHAR NOT NULL,
	        environment     VARCHAR,
	        params_json     VARCHAR NOT NULL,
	        workbook_id     VARCHAR,
	        parent_run_id   VARCHAR,
	        user_name       VARCHAR,
	        status          VARCHAR NOT NULL CHECK (status IN ('queued', 'running', 'succeeded', 'failed', 'cancelled')),
	        started_at      TIMESTAMP NOT NULL,
	        finished_at     TIMESTAMP,
	        rows_fetched    BIGINT NOT NULL DEFAULT 0,
	        truncated       BOOLEAN NOT NULL DEFAULT false,
	        result_table    VARCHAR,
	        warnings_json   VARCHAR NOT NULL DEFAULT '[]',
	        error_code      VARCHAR,
	        error_message   VARCHAR))",
	    R"(CREATE TABLE meta.trusted_definitions (
	        definition_hash VARCHAR PRIMARY KEY,
	        query_id        VARCHAR NOT NULL,
	        origin          VARCHAR NOT NULL CHECK (origin IN ('local_editor', 'user_consent', 'example')),
	        workbook_id     VARCHAR,
	        trusted_at      TIMESTAMP NOT NULL))",
	    R"(CREATE TABLE meta.missing (
	        workbook_id     VARCHAR NOT NULL,
	        instance_key    VARCHAR NOT NULL,
	        query_id        VARCHAR NOT NULL,
	        definition_hash VARCHAR NOT NULL,
	        connection_name VARCHAR NOT NULL,
	        params_json     VARCHAR NOT NULL,
	        first_seen      TIMESTAMP NOT NULL,
	        last_seen       TIMESTAMP NOT NULL,
	        requests        BIGINT NOT NULL,
	        PRIMARY KEY (workbook_id, instance_key)))",
	    R"(CREATE TABLE meta.imports (
	        name         VARCHAR PRIMARY KEY,
	        workbook_id  VARCHAR NOT NULL,
	        source       VARCHAR NOT NULL,
	        schema_json  VARCHAR NOT NULL,
	        row_count    BIGINT NOT NULL,
	        imported_at  TIMESTAMP NOT NULL,
	        data_version BIGINT NOT NULL))",
	    "INSERT INTO meta.state VALUES ('data_version', '0'), ('catalog_version', '')",
	};
	return statements;
}

void ApplyVersion(duckdb::Connection &connection, int version, const std::vector<const char *> &statements) {
	connection.BeginTransaction();
	try {
		for (const auto *statement : statements) {
			Execute(connection, statement);
		}
		Execute(connection, "DELETE FROM meta.schema_version");
		Execute(connection, "INSERT INTO meta.schema_version VALUES (" + std::to_string(version) + ")");
		connection.Commit();
	} catch (...) {
		connection.Rollback();
		throw;
	}
}

} // namespace

int CurrentSchemaVersion(duckdb::Connection &connection) {
	const auto exists = QueryRows(connection, "SELECT count(*) FROM duckdb_tables() "
	                                          "WHERE schema_name = 'meta' AND table_name = 'schema_version'");
	if (exists.empty() || exists[0][0].GetValue<int64_t>() == 0) {
		return 0;
	}
	const auto rows = QueryRows(connection, "SELECT max(version) FROM meta.schema_version");
	if (rows.empty() || rows[0][0].IsNull()) {
		return 0;
	}
	return rows[0][0].GetValue<int32_t>();
}

void RunMigrations(duckdb::Connection &connection) {
	Execute(connection, "CREATE SCHEMA IF NOT EXISTS meta");
	Execute(connection, "CREATE TABLE IF NOT EXISTS meta.schema_version (version INTEGER NOT NULL)");
	const auto current = CurrentSchemaVersion(connection);
	if (current > LATEST_SCHEMA_VERSION) {
		throw duckdb::IOException("E_INTERNAL: the database was created by a newer version of ofexceld (schema %d, "
		                          "this build knows only %d)",
		                          current, LATEST_SCHEMA_VERSION);
	}
	if (current < 1) {
		ApplyVersion(connection, 1, SchemaV1());
	}
	// A new version is added here as `if (current < 2) ApplyVersion(connection, 2, SchemaV2());`
	// and never by editing an earlier one: databases already at that version
	// would not see the change.
}

MetaState ReadMetaState(duckdb::Connection &connection) {
	MetaState state;
	for (const auto &row : QueryRows(connection, "SELECT key, value FROM meta.state")) {
		const auto key = row[0].ToString();
		if (key == "data_version") {
			state.data_version = std::stoll(row[1].ToString());
		} else if (key == "catalog_version") {
			state.catalog_version = row[1].ToString();
		}
	}
	return state;
}

} // namespace ofexcel
