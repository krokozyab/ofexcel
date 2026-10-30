#pragma once

// CONTRACTS.md §14.1 for errors from an Oracle source: oracle_fusion_query and
// ofquack's SSO functions. Matching on the start of ofquack's messages is
// temporary, until ofquack has stable codes (§15, Q1).

#include "duckdb.hpp"
#include "ofexcel/api_error.hpp"

namespace ofexcel {

//! The prefix of ofquack's IOException for a query without rows (§5.2). A
//! caller that treats an empty result specially checks it before mapping.
constexpr const char *FUSION_NO_ROWS_PREFIX = "Oracle Fusion returned no rows";

bool IsFusionNoRows(const duckdb::ErrorData &error);

//! E_FUSION_AUTH, E_FUSION_REJECTED, E_FUSION_UNAVAILABLE, E_OUT_OF_MEMORY or
//! E_INTERNAL, with the §14 status and a message for the user. Interrupts are
//! not mapped here: whether one is a cancellation or a timeout only the run
//! knows. Logs the original text to stderr, where the details belong.
ApiError FusionApiError(const duckdb::ErrorData &error);

} // namespace ofexcel
