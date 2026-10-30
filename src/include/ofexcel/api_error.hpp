#pragma once

// An error with a CONTRACTS.md §14 code. Thrown by code that both an HTTP route
// and a SQL function call: the route sends `status` and an ErrorBody, the SQL
// function rethrows it as a DuckDB exception whose message starts with `code`.

#include "ofexcel/json_body.hpp"

#include <memory>
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

	//! With ErrorBody's `details`, for an error that has parts: the queries of
	//! a workbook that did not parse, each with its own errors.
	ApiError(std::string code_p, int status_p, const std::string &message, JsonObject details_p)
	    : ApiError(std::move(code_p), status_p, message) {
		details = std::make_shared<const JsonObject>(std::move(details_p));
	}

	std::string code;
	int status;
	//! Shared, so the exception stays copyable; null when there are none.
	std::shared_ptr<const JsonObject> details;
};

} // namespace ofexcel
