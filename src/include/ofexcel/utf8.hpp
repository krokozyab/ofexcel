#pragma once

// UTF-8 helpers shared by the pure modules. No DuckDB.

#include <cstddef>
#include <string>

namespace ofexcel {

//! Strict UTF-8: no overlong forms, no surrogates, nothing above U+10FFFF.
bool IsValidUtf8(const std::string &text);

//! The number of code points in `text`, which must be valid UTF-8.
size_t Utf8Length(const std::string &text);

} // namespace ofexcel
