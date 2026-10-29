// Local SQL result types: DuckDB type → contract type (CONTRACTS.md §3.4), a
// check per row of the table. Links DuckDB for LogicalType, opens no database.

#include "ofexcel/local_types.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

using namespace ofexcel;
using duckdb::LogicalType;

namespace {

int failures = 0;

void Expect(const char *id, const LogicalType &type, const std::string &name, bool large, const std::string &expected,
            bool cast = false) {
	LocalColumnStats stats;
	stats.has_value_from_1e15 = large;
	const auto mapping = MapLocalType(type, name, stats);
	if (mapping.type.ToString() != expected || mapping.cast_to_varchar != cast || mapping.warning.empty() == cast) {
		std::cerr << id << ": " << type.ToString() << " " << name << " → " << mapping.type.ToString()
		          << (mapping.cast_to_varchar ? " (cast)" : "") << ", expected " << expected << (cast ? " (cast)" : "")
		          << "\n";
		failures++;
	}
}

} // namespace

int main() {
	Expect("VARCHAR", LogicalType::VARCHAR, "NAME", false, "string");
	Expect("BOOLEAN", LogicalType::BOOLEAN, "FLAG", false, "boolean");
	for (const LogicalType &type :
	     std::vector<LogicalType> {LogicalType::TINYINT, LogicalType::SMALLINT, LogicalType::INTEGER, LogicalType::BIGINT,
	                               LogicalType::UTINYINT, LogicalType::USMALLINT, LogicalType::UINTEGER,
	                               LogicalType::UBIGINT, LogicalType::HUGEINT, LogicalType::UHUGEINT}) {
		Expect("integer family", type, "AMOUNT", false, "integer");
		Expect("integer family _ID", type, "vendor_id", false, "id");
		Expect("integer family >= 10^15", type, "AMOUNT", true, "id");
	}
	Expect("DECIMAL", LogicalType::DECIMAL(18, 2), "AMOUNT", false, "decimal(18,2)");
	Expect("DECIMAL(38,0)", LogicalType::DECIMAL(38, 0), "AMOUNT", false, "decimal(38,0)");
	Expect("FLOAT", LogicalType::FLOAT, "X", false, "number");
	Expect("DOUBLE", LogicalType::DOUBLE, "X", false, "number");
	Expect("DATE", LogicalType::DATE, "D", false, "date");
	for (const LogicalType &type : std::vector<LogicalType> {LogicalType::TIMESTAMP, LogicalType::TIMESTAMP_MS,
	                                                         LogicalType::TIMESTAMP_NS, LogicalType::TIMESTAMP_S}) {
		Expect("TIMESTAMP family", type, "TS", false, "timestamp");
	}
	Expect("TIMESTAMP WITH TIME ZONE", LogicalType::TIMESTAMP_TZ, "TZ", false, "timestamptz");
	for (const LogicalType &type : std::vector<LogicalType> {LogicalType::LIST(LogicalType::INTEGER), LogicalType::BLOB,
	                                                         LogicalType::INTERVAL, LogicalType::UUID, LogicalType::TIME}) {
		Expect("other", type, "X", false, "string", true);
	}
	if (failures) {
		std::cerr << failures << " local type check(s) failed\n";
		return EXIT_FAILURE;
	}
	std::cout << "local_types_test: all checks passed\n";
	return EXIT_SUCCESS;
}
