#pragma once

// The contract type of a Local SQL result column from its DuckDB type
// (CONTRACTS.md §3.4). The one piece of the type contract that knows DuckDB;
// everything else is in src/catalog/.

#include "duckdb.hpp"
#include "ofexcel/contract_types.hpp"

#include <string>

namespace ofexcel {

//! What the caller learned from the column's values.
struct LocalColumnStats {
	//! An integer column holds a value with |value| >= 10^15.
	bool has_value_from_1e15 = false;
};

struct LocalColumnMapping {
	ColumnType type;
	//! The column is not one §3.4 lists: the run stores CAST(… AS VARCHAR)
	//! and reports `warning`.
	bool cast_to_varchar = false;
	std::string warning;
};

LocalColumnMapping MapLocalType(const duckdb::LogicalType &type, const std::string &name, const LocalColumnStats &stats);

} // namespace ofexcel
