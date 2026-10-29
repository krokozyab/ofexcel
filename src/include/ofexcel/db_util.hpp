#pragma once

// Small helpers over DuckDB 2.0's result API, which hands back rows chunk by
// chunk with no (column, row) access on the result itself.

#include "duckdb.hpp"

#include <string>
#include <vector>

namespace ofexcel {

using Row = std::vector<duckdb::Value>;

//! Runs one or more statements and throws duckdb::IOException with the
//! database's message if any of them fails. For DDL and DML.
void Execute(duckdb::Connection &connection, const std::string &sql);

//! Runs a query and returns every row. For metadata-sized results only: it
//! holds the whole result in memory.
std::vector<Row> QueryRows(duckdb::Connection &connection, const std::string &sql);

} // namespace ofexcel
