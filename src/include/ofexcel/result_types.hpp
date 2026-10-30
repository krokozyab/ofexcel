#pragma once

// Types of result columns (CONTRACTS.md §3.2, §3.3): what type an Oracle
// column gets from its values, the text each value is stored as, and how a
// stored value is sent to the add-in. Pure: no DuckDB. The Local side's
// DuckDB-type mapping is src/run/local_types.cpp.

#include "ofexcel/contract_types.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace ofexcel {

struct ColumnInference {
	bool ok = true;
	ColumnType type;
	//! The start type did not hold the values and was widened (§3.3 item 4).
	bool widened = false;
	//! English, for the run's warnings.
	std::vector<std::string> warnings;

	//! !ok: E_TYPE_DECLARED_MISMATCH, the 1-based row and the value that does
	//! not fit the @column type, and a message naming all three.
	std::string code;
	int64_t row = 0;
	std::string value;
	std::string message;
};

//! The type of one Oracle result column, decided over the whole result
//! (§3.3): values are added row by row, so a million rows need not be held.
class OracleColumnInference {
public:
	//! `declared` (@column) and `start` (meta.query_schemas) may be null;
	//! `declared` wins.
	OracleColumnInference(std::string name, const ColumnType *declared, const ColumnType *start);

	//! The next row's value as ofquack sent it with all_varchar. "" is NULL:
	//! Oracle does not tell them apart (§3.3).
	void Add(const std::string &value);

	ColumnInference Finish() const;

	//! What every non-NULL value seen so far has in common.
	struct Summary {
		int64_t values = 0;
		bool all_integer = true;      //!< ^-?(0|[1-9][0-9]*)$
		size_t max_integer_digits = 0;
		bool all_decimal = true;      //!< ^-?(0|[1-9][0-9]*)?(\.[0-9]+)?$
		size_t max_whole_digits = 0;  //!< before the point, leading zeros dropped
		size_t max_scale = 0;         //!< after the point, trailing zeros dropped
		bool all_number = true;       //!< decimal, exponent form, Inf, NaN
		bool all_date = true;         //!< YYYY-MM-DD
		bool all_timestamp = true;    //!< TS
		bool all_midnight = true;     //!< TS at 00:00:00 with a zero fraction
		bool all_date_or_midnight = true;  //!< what a date column holds
		bool all_date_or_timestamp = true; //!< what a timestamp column holds
		bool all_timestamptz = true;  //!< TSTZ
		bool all_boolean = true;      //!< Y N TRUE FALSE 1 0, any case
	};

private:
	std::string name;
	bool has_declared = false;
	ColumnType declared;
	bool has_start = false;
	ColumnType start;
	int64_t rows = 0;
	Summary summary;
	int64_t mismatch_row = 0;
	std::string mismatch_value;
};

enum class StoredValue { VALUE, NULL_VALUE, MISMATCH };

//! The text to store for an Oracle value in a column of `type`, in a form
//! DuckDB casts to the column's storage type (§3.2): ".84" → "0.84", a
//! timestamp's fraction cut to microseconds (not rounded), a TSTZ moved to UTC.
//! "" is NULL_VALUE; a value `type` does not hold is MISMATCH.
StoredValue ConvertOracleValue(const ColumnType &type, const std::string &text, std::string &stored);

//! One value for a JSON array, typed so that the caller adds it through
//! JsonArrayRef without writing JSON by hand.
struct JsonCell {
	enum class Kind { NULL_VALUE, STRING, INTEGER, NUMBER, BOOLEAN };
	Kind kind = Kind::NULL_VALUE;
	std::string text;
	int64_t integer = 0;
	double number = 0;
	bool boolean = false;
	//! A NaN or infinite number sent as null; the run reports a warning (§3.2).
	bool non_finite = false;
};

//! The JSON form (§3.2) of a stored value: `text` is the value as DuckDB
//! prints it (Value::ToString), or null for NULL.
JsonCell EncodeJsonValue(const ColumnType &type, const std::string *text);

} // namespace ofexcel
