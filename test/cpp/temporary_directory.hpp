#pragma once

// A fresh directory under the system temporary directory, removed with
// everything in it when the object goes. Shared by the pure tests and the ones
// that link DuckDB, so it includes nothing but the standard library.

#include <filesystem>
#include <random>
#include <string>
#include <system_error>

namespace test_support {

class TemporaryDirectory {
public:
	explicit TemporaryDirectory(const std::string &prefix) {
		std::random_device random;
		const auto name = prefix + "_" + std::to_string(random()) + std::to_string(random());
		path = (std::filesystem::temp_directory_path() / name).u8string();
	}
	~TemporaryDirectory() {
		std::error_code error;
		std::filesystem::remove_all(std::filesystem::u8path(path), error);
	}
	TemporaryDirectory(const TemporaryDirectory &) = delete;
	TemporaryDirectory &operator=(const TemporaryDirectory &) = delete;

	//! UTF-8. Not created: the code under test creates it, or a test does.
	std::string path;
};

} // namespace test_support
