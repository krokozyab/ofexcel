#pragma once

// Shared by the C++ tests that link DuckDB. Header-only on purpose: each test
// is one executable, and a support library would be one more target to wire.

// Before any include. Windows defines min and max as function-like macros, and
// the headers pulled in below reach <windows.h>.
#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX
#endif

#include "core_functions_extension.hpp"
#include "duckdb.hpp"
#include "fusion_scanner_extension.hpp"
#include "ofexcel_extension.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

[[noreturn]] inline void CheckFailed(const char *expression, const char *file, int line) {
	std::cerr << file << ":" << line << ": check failed: " << expression << std::endl;
	std::abort();
}

#define CHECK(expression)                                                                                              \
	do {                                                                                                               \
		if (!(expression)) {                                                                                           \
			CheckFailed(#expression, __FILE__, __LINE__);                                                              \
		}                                                                                                              \
	} while (0)

namespace test_support {

//! An in-memory database with the three extensions ofexceld links. DuckDB 2.0
//! loads nothing by itself, not even list_value.
class TestDatabase : public duckdb::DuckDB {
public:
	TestDatabase() : duckdb::DuckDB(nullptr) {
		LoadStaticExtension<duckdb::CoreFunctionsExtension>();
		LoadStaticExtension<duckdb::FusionScannerExtension>();
		LoadStaticExtension<duckdb::OfexcelExtension>();
	}
};

//! A whole result read up front, or the error -- whether it surfaced when the
//! query ran or while its rows were fetched.
struct Result {
	std::string error;
	std::vector<std::string> names;
	std::vector<duckdb::LogicalType> types;
	std::vector<std::vector<duckdb::Value>> rows;

	bool HasError() const {
		return !error.empty();
	}
	const duckdb::Value &At(size_t row, size_t column) const {
		return rows.at(row).at(column);
	}
};

inline Result Run(duckdb::Connection &connection, const std::string &sql) {
	Result collected;
	auto result = connection.Query(sql);
	if (result->HasError()) {
		collected.error = result->GetError();
		return collected;
	}
	collected.types = result->GetTypes();
	for (const auto &name : result->GetNames()) {
		collected.names.push_back(name.GetIdentifierName());
	}
	try {
		while (auto chunk = result->Fetch()) {
			if (chunk->size() == 0) {
				break;
			}
			for (duckdb::idx_t row = 0; row < chunk->size(); row++) {
				std::vector<duckdb::Value> values;
				for (duckdb::idx_t column = 0; column < collected.types.size(); column++) {
					values.push_back(chunk->GetValue(column, row));
				}
				collected.rows.push_back(std::move(values));
			}
		}
	} catch (const std::exception &exception) {
		collected.error = duckdb::ErrorData(exception).Message();
		collected.rows.clear();
	}
	if (result->HasError() && collected.error.empty()) {
		collected.error = result->GetError();
		collected.rows.clear();
	}
	return collected;
}

//! Runs `sql` and aborts the test with the database's message if it fails.
inline Result RunOk(duckdb::Connection &connection, const std::string &sql) {
	auto result = Run(connection, sql);
	if (result.HasError()) {
		std::cerr << "query failed: " << sql << "\n" << result.error << std::endl;
		std::abort();
	}
	return result;
}

inline bool Contains(const std::string &haystack, const std::string &needle) {
	return haystack.find(needle) != std::string::npos;
}

} // namespace test_support
