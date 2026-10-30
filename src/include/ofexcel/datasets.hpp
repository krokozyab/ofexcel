#pragma once

// Where results are stored (CONTRACTS.md §6.1–6.4): dataset instance and stage
// table names, the DuckDB type each contract type is stored as, the schema a
// query's earlier runs left in meta.query_schemas, and the cleanup ofexceld
// does at start.

#include "duckdb.hpp"
#include "ofexcel/contract_types.hpp"

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace ofexcel {

//! The schema dataset instances and stage tables live in.
constexpr const char *DATASET_SCHEMA = "ds";
//! ofexcel_dataset_ttl_days' default.
constexpr int64_t DEFAULT_DATASET_TTL_DAYS = 30;

//! `name` as a quoted SQL identifier: "a""b".
std::string QuoteIdentifier(const std::string &name);

//! ds.<query_id>__<first 12 characters of instance_key>__v<version>, without
//! the schema (§6.1).
std::string InstanceTableName(const std::string &query_id, const std::string &instance_key, int64_t version);

//! stage__<run_id without dashes> (§6.1).
std::string StageTableName(const std::string &run_id);

//! The DuckDB type a column of `type` is stored as (§3.2).
duckdb::LogicalType StorageType(const ColumnType &type);

//! One column of a query's known schema.
struct SchemaColumn {
	std::string name;
	ColumnType type;
	//! From @column; "" when there is none.
	std::string header;
};

//! meta.query_schemas for `definition_hash`, by ordinal; empty when no run has
//! stored one yet. A type that no longer parses is read as string.
std::vector<SchemaColumn> ReadQuerySchema(duckdb::Connection &connection, const std::string &definition_hash);

//! Replaces meta.query_schemas for `definition_hash`. The caller holds a
//! transaction.
void WriteQuerySchema(duckdb::Connection &connection, const std::string &definition_hash,
                      const std::vector<SchemaColumn> &columns);

//! Serializes the writes that touch rows other runs write too: meta.state's
//! data_version, an instance's row, a definition's schema. DuckDB refuses the
//! second of two transactions that update the same row, rather than waiting.
std::mutex &MetaWriteLock();

struct DatasetCleanup {
	int64_t expired_instances = 0;
	int64_t stage_tables = 0;
	int64_t interrupted_runs = 0;
};

//! At start (§6.4): drops instances not read for `ttl_days` days with their
//! tables, stage tables a stopped daemon left behind, and marks runs that were
//! still going as failed with E_INTERNAL.
DatasetCleanup CleanupDatasets(duckdb::Connection &connection, int64_t ttl_days);

} // namespace ofexcel
