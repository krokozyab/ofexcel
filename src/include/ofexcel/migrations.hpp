#pragma once

#include "duckdb.hpp"

#include <cstdint>
#include <string>

namespace ofexcel {

//! The schema version this build creates (CONTRACTS.md §6.2).
constexpr int LATEST_SCHEMA_VERSION = 1;

//! Brings meta.*, ds and xl_import up to LATEST_SCHEMA_VERSION. Idempotent:
//! a database already at the latest version is left alone. Each version is
//! applied in its own transaction.
void RunMigrations(duckdb::Connection &connection);

//! 0 for a database that has never been migrated.
int CurrentSchemaVersion(duckdb::Connection &connection);

struct MetaState {
	int64_t data_version = 0;
	std::string catalog_version;
};

//! The counters every /v1 response reports in X-OF-Data-Version and
//! X-OF-Catalog-Version. Requires a migrated database.
MetaState ReadMetaState(duckdb::Connection &connection);

} // namespace ofexcel
