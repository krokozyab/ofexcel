#pragma once

// The contract's parameter and column types (CONTRACTS.md §2.2, §3.1): how a
// type is written in a query header, and the canonical form of a value given
// as text. Pure: no DuckDB.
//
// Text is one of the input forms §3.1 accepts, and the only one a query
// header has: defaults are written as text. NormalizeParam for JSON input
// (TASKS.md B3) builds on CanonicalizeText.

#include <memory>
#include <string>
#include <vector>

namespace ofexcel {

enum class ParamKind { INTEGER, NUMBER, DECIMAL, STRING, ID, DATE, TIMESTAMP, BOOLEAN, ENUM, LIST };

struct ParamType {
	ParamKind kind = ParamKind::STRING;
	//! DECIMAL: 1 <= precision <= 38, 0 <= scale <= precision.
	int precision = 0;
	int scale = 0;
	//! ENUM: the variants as declared; unique without regard to case.
	std::vector<std::string> variants;
	//! LIST: the element type, never LIST or ENUM.
	std::shared_ptr<const ParamType> element;

	//! The type as a header would write it, without spaces: "decimal(18,2)",
	//! "enum(OPEN,PAID)", "list<id>".
	std::string ToString() const;
};

enum class ColumnKind { ID, INTEGER, DECIMAL, NUMBER, STRING, TEXT, DATE, TIMESTAMP, TIMESTAMPTZ, BOOLEAN };

struct ColumnType {
	ColumnKind kind = ColumnKind::STRING;
	int precision = 0;
	int scale = 0;

	std::string ToString() const;
};

//! Parses `ptype` from §2.2's grammar; spaces between lexemes are allowed,
//! type names are lower case. Returns false and sets `error` (Russian, for
//! the user) if `text` is not one.
bool ParseParamType(const std::string &text, ParamType &type, std::string &error);

//! Parses `ctype` from §2.2's grammar.
bool ParseColumnType(const std::string &text, ColumnType &type, std::string &error);

//! The canonical form (§3.1) of `text` as a value of the scalar `type`.
//! Returns false and sets `error` if §3.1 refuses it. `type` must not be LIST.
bool CanonicalizeText(const ParamType &type, const std::string &text, std::string &canonical, std::string &error);

//! The shortest decimal text that reads back as `value`, in std::to_chars'
//! plain format: fixed or scientific, whichever is shorter, fixed on a tie.
//! -0 is written "0". `value` must be finite. (std::to_chars for double needs
//! macOS 13.3, and the build targets 11.0.)
std::string ShortestDouble(double value);

} // namespace ofexcel
