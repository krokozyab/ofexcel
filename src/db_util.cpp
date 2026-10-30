#include "ofexcel/db_util.hpp"

namespace ofexcel {

namespace {

//! Every row of `result`, or its error. An error can surface while rows are
//! fetched, not only when the query runs.
QueryOutcome Collect(duckdb::QueryResult &result) {
	QueryOutcome outcome;
	if (result.HasError()) {
		outcome.error = result.GetErrorObject();
		return outcome;
	}
	const auto column_count = result.GetTypes().size();
	try {
		while (auto chunk = result.Fetch()) {
			if (chunk->size() == 0) {
				break;
			}
			for (duckdb::idx_t row = 0; row < chunk->size(); row++) {
				Row values;
				values.reserve(column_count);
				for (duckdb::idx_t column = 0; column < column_count; column++) {
					values.push_back(chunk->GetValue(column, row));
				}
				outcome.rows.push_back(std::move(values));
			}
		}
	} catch (const std::exception &exception) {
		outcome.error = duckdb::ErrorData(exception);
	}
	if (!outcome.error.HasError() && result.HasError()) {
		outcome.error = result.GetErrorObject();
	}
	if (outcome.error.HasError()) {
		outcome.rows.clear();
	}
	return outcome;
}

std::vector<Row> RowsOrThrow(QueryOutcome outcome) {
	if (!outcome.Ok()) {
		throw duckdb::IOException("%s", outcome.error.Message());
	}
	return std::move(outcome.rows);
}

} // namespace

void Execute(duckdb::Connection &connection, const std::string &sql) {
	auto result = connection.Query(sql);
	if (result->HasError()) {
		throw duckdb::IOException("%s", result->GetError());
	}
}

std::vector<Row> QueryRows(duckdb::Connection &connection, const std::string &sql) {
	auto result = connection.Query(sql);
	return RowsOrThrow(Collect(*result));
}

QueryOutcome TryQueryRows(duckdb::Connection &connection, const std::string &sql,
                          duckdb::vector<duckdb::Value> params) {
	auto prepared = connection.Prepare(sql);
	if (prepared->HasError()) {
		QueryOutcome outcome;
		outcome.error = prepared->GetErrorObject();
		return outcome;
	}
	auto result = prepared->Execute(params);
	return Collect(*result);
}

std::vector<Row> QueryRows(duckdb::Connection &connection, const std::string &sql,
                           duckdb::vector<duckdb::Value> params) {
	return RowsOrThrow(TryQueryRows(connection, sql, std::move(params)));
}

} // namespace ofexcel
