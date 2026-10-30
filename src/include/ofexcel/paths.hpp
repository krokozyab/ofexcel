#pragma once

// Where ofexceld keeps its files on disk (CONTRACTS.md §1). Pure: no DuckDB.
//
// Paths are UTF-8 strings on every platform. On Windows, open them through
// std::filesystem::u8path, never as narrow paths: the user's profile directory
// can hold characters outside the ANSI code page.

#include <string>

namespace ofexcel {

//! The environment variable QEXCEL_HOME if set; otherwise macOS
//! ~/Library/Application Support/QExcel, Windows %LOCALAPPDATA%\QExcel,
//! elsewhere $XDG_DATA_HOME/QExcel or ~/.local/share/QExcel. Throws
//! std::runtime_error if the home directory cannot be determined.
std::string QexcelHome();

//! QexcelHome()/tls: the local CA and the server certificate (CONTRACTS.md §8.1).
std::string DefaultTlsDir();

//! ~/QExcel/projects (Windows %USERPROFILE%\QExcel\projects): one directory per
//! project, its query files under queries/ (CONTRACTS.md §1). Not under
//! QEXCEL_HOME: these are the author's files, kept where the author sees them.
//! Throws std::runtime_error if the home directory cannot be determined.
std::string DefaultProjectsDir();

//! `directory` and `name` joined with the platform's separator.
std::string JoinPath(const std::string &directory, const std::string &name);

} // namespace ofexcel
