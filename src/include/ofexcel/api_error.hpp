#pragma once

// An error with a CONTRACTS.md §14 code. Thrown by code that both an HTTP route
// and a SQL function call: the route sends `status` and an ErrorBody, the SQL
// function rethrows it as a DuckDB exception whose message starts with `code`.

#include <stdexcept>
#include <string>
#include <utility>

namespace ofexcel {

class ApiError : public std::runtime_error {
public:
	//! `message` is shown to the user, so it is in Russian.
	ApiError(std::string code_p, int status_p, const std::string &message)
	    : std::runtime_error(message), code(std::move(code_p)), status(status_p) {
	}

	std::string code;
	int status;
};

} // namespace ofexcel
