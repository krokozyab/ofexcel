#pragma once

// Formulas (CONTRACTS.md §10; TASKS.md C8): OF.TABLE, OF.CALL and OF.ASOF
// evaluated a batch at a time (/v1/batch, ofexcel_call), and the core's
// functions (functions.json, /v1/catalog). A formula never runs a query: it
// reads the instance a run stored, or evaluates a Local query over its
// dependencies' current data; what is missing goes to meta.missing.

#include "duckdb.hpp"
#include "ofexcel/json_body.hpp"
#include "ofexcel/param_values.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace ofexcel {

//! The core's functions.json (§10.1): served at /addin/functions.json and
//! listed by /v1/catalog.
const std::string &CoreFunctionsJson();

//! An argument as Excel passes it: a matrix of cells, row by row.
using CellMatrix = std::vector<std::vector<ParamInput>>;

struct FormulaCall {
	int64_t i = 0;
	//! table, call or asof.
	std::string kind;
	std::string query;
	std::vector<CellMatrix> args;
};

//! What the workbook a batch comes from tells.
struct BatchContext {
	//! X-OF-Workbook; "" for none: then only projects and examples resolve,
	//! and nothing is recorded in meta.missing.
	std::string workbook_id;
	//! The workbook's default connection; "" takes @connection.
	std::string connection;
	//! The workbook's date system: serial numbers count from 1904-01-01
	//! instead of 1899-12-30 (§11.4).
	bool date1904 = false;
};

//! What a cell of the result shows in Excel (§11.4).
struct ExcelValue {
	enum class Kind { STRING, NUMBER, BOOLEAN };
	Kind kind = Kind::STRING;
	std::string text;
	double number = 0;
	bool boolean = false;
};

struct FormulaResult {
	int64_t i = 0;
	bool ok = false;
	//! OF.TABLE: headers, then rows. OF.CALL and OF.ASOF: one value.
	bool matrix = false;
	std::vector<std::vector<ExcelValue>> rows;
	//! !ok: invalidValue, invalidName or notAvailable (§14's Excel column).
	std::string error;
	std::string message;
};

//! Evaluates `calls` (§10.2, §10.3). Each gets its value or its error, in the
//! order given; only a database failure throws.
std::vector<FormulaResult> EvaluateBatch(duckdb::DatabaseInstance &db, const BatchContext &context,
                                         const std::vector<FormulaCall> &calls);

//! A result as /v1/batch sends it: {i, v} or {i, e: {code, msg}}; without
//! `i` for ofexcel_call.
JsonObject FormulaResultJson(const FormulaResult &result, bool with_number = true);

//! `args` of a call from JSON: an array of matrices. Throws ApiError
//! E_BAD_REQUEST (422) for anything else; `call` names the call in the message.
std::vector<CellMatrix> FormulaArgs(const JsonValue &args, const std::string &call);

//! Forgets every cached result (tests).
void ClearFormulaCache();

} // namespace ofexcel
