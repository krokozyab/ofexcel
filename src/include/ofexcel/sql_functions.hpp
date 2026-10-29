#pragma once

#include "duckdb.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

namespace ofexcel {

//! The ofexcel_* settings (CONTRACTS.md §13).
void RegisterSettings(duckdb::ExtensionLoader &loader);

//! The ofexcel_* SQL functions (CONTRACTS.md §13).
void RegisterSqlFunctions(duckdb::ExtensionLoader &loader);

} // namespace ofexcel
