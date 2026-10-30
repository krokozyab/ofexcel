#include "ofexcel/paths.hpp"

#include <cstdlib>
#include <stdexcept>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace ofexcel {

namespace {

#ifdef _WIN32
//! The variable as UTF-8; "" if unset. _wgetenv, because getenv returns the
//! value in the ANSI code page.
std::string EnvironmentVariable(const wchar_t *name) {
	const wchar_t *value = _wgetenv(name);
	if (!value || !*value) {
		return "";
	}
	const int size = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
	std::string utf8(static_cast<size_t>(size), '\0');
	WideCharToMultiByte(CP_UTF8, 0, value, -1, &utf8[0], size, nullptr, nullptr);
	utf8.resize(static_cast<size_t>(size) - 1);
	return utf8;
}
#else
std::string EnvironmentVariable(const char *name) {
	const char *value = std::getenv(name);
	return value ? value : "";
}
#endif

} // namespace

std::string JoinPath(const std::string &directory, const std::string &name) {
#ifdef _WIN32
	const char separator = '\\';
#else
	const char separator = '/';
#endif
	if (directory.empty() || directory.back() == '/' || directory.back() == separator) {
		return directory + name;
	}
	return directory + separator + name;
}

std::string QexcelHome() {
#ifdef _WIN32
	const auto overridden = EnvironmentVariable(L"QEXCEL_HOME");
#else
	const auto overridden = EnvironmentVariable("QEXCEL_HOME");
#endif
	if (!overridden.empty()) {
		return overridden;
	}
#ifdef _WIN32
	const auto local_app_data = EnvironmentVariable(L"LOCALAPPDATA");
	if (local_app_data.empty()) {
		throw std::runtime_error("The LOCALAPPDATA environment variable is not set: cannot find the QExcel folder");
	}
	return JoinPath(local_app_data, "QExcel");
#else
	const auto home = EnvironmentVariable("HOME");
	if (home.empty()) {
		throw std::runtime_error("The HOME environment variable is not set: cannot find the QExcel folder");
	}
#ifdef __APPLE__
	return JoinPath(home, "Library/Application Support/QExcel");
#else
	const auto data_home = EnvironmentVariable("XDG_DATA_HOME");
	return JoinPath(data_home.empty() ? JoinPath(home, ".local/share") : data_home, "QExcel");
#endif
#endif
}

std::string DefaultTlsDir() {
	return JoinPath(QexcelHome(), "tls");
}

std::string DefaultProjectsDir() {
#ifdef _WIN32
	const auto home = EnvironmentVariable(L"USERPROFILE");
#else
	const auto home = EnvironmentVariable("HOME");
#endif
	if (home.empty()) {
		throw std::runtime_error("The user's home folder was not found: cannot find the QExcel projects folder");
	}
	return JoinPath(JoinPath(home, "QExcel"), "projects");
}

} // namespace ofexcel
