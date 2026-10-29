#include "ofexcel/db_util.hpp"

namespace ofexcel {

void Execute(duckdb::Connection &connection, const std::string &sql) {
	auto result = connection.Query(sql);
	if (result->HasError()) {
		throw duckdb::IOException("%s", result->GetError());
	}
}

std::vector<Row> QueryRows(duckdb::Connection &connection, const std::string &sql) {
	auto result = connection.Query(sql);
	if (result->HasError()) {
		throw duckdb::IOException("%s", result->GetError());
	}
	const auto column_count = result->GetTypes().size();
	std::vector<Row> rows;
	while (auto chunk = result->Fetch()) {
		if (chunk->size() == 0) {
			break;
		}
		for (duckdb::idx_t row = 0; row < chunk->size(); row++) {
			Row values;
			values.reserve(column_count);
			for (duckdb::idx_t column = 0; column < column_count; column++) {
				values.push_back(chunk->GetValue(column, row));
			}
			rows.push_back(std::move(values));
		}
	}
	// An error can surface while rows are fetched, not only when the query runs.
	if (result->HasError()) {
		throw duckdb::IOException("%s", result->GetError());
	}
	return rows;
}

} // namespace ofexcel
