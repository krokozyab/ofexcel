#pragma once

#include <string>

namespace ofexcel {

//! Product version, reported by /v1/health and ofexcel_version().
extern const char *const PRODUCT_VERSION;

//! HTTP API version and the oldest client version it serves (CONTRACTS.md §8.3).
extern const char *const API_VERSION;
extern const char *const API_MIN_CLIENT;

//! PRODUCT_VERSION and when this binary was compiled. DuckDB keeps a loaded
//! extension for the life of the process, so this is how to tell which build is
//! actually in memory.
std::string VersionWithBuildStamp();

} // namespace ofexcel
