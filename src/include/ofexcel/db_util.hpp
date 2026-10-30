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

//! The same with `params` bound to $1, $2, … of a prepared statement. Values
//! from a request go here, never into the SQL text.
std::vector<Row> QueryRows(duckdb::Connection &connection, const std::string &sql,
                           duckdb::vector<duckdb::Value> params);

struct QueryOutcome {
	std::vector<Row> rows;
	//! HasError() when the statement failed, with the exception type intact:
	//! CONTRACTS.md §14.1 maps errors by type, which a rethrown message loses.
	duckdb::ErrorData error;

	bool Ok() const {
		return !error.HasError();
	}
};

//! QueryRows with parameters that returns the error instead of throwing it.
QueryOutcome TryQueryRows(duckdb::Connection &connection, const std::string &sql,
                          duckdb::vector<duckdb::Value> params);

} // namespace ofexcel
