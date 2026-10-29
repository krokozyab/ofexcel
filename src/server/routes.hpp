#pragma once

// What a route file needs: httplib, the database, and the helpers that make
// every response look the same. Internal to src/server; not an installed header.
//
// Adding a route group: create src/server/routes_<group>.cpp with a
// Register<Group>Routes function, declare it below, call it from
// RegisterRoutes in http_server.cpp, add the file to CMakeLists.txt.

#include "duckdb.hpp"
#include "ofexcel/http_server.hpp"

#include <httplib.h>

#include <string>

namespace ofexcel {

struct RouteContext {
	//! Weak: the server must not keep the database alive after its owner closed it.
	duckdb::weak_ptr<duckdb::DatabaseInstance> db;
	ServerOptions options;

	//! A fresh connection for this request, or nullptr if the database is gone.
	//! A connection is never shared between requests: httplib runs handlers on
	//! a thread pool, and a duckdb::Connection is not safe across threads.
	duckdb::unique_ptr<duckdb::Connection> Connect() const;
};

//! Sets the body and Content-Type: application/json; charset=utf-8.
void SendJson(httplib::Response &response, int status, const std::string &body);

//! Sends ErrorBody(code, message) with `status`. `message` is shown to the user,
//! so it is written in Russian (CONTRACTS.md §8.1).
void SendError(httplib::Response &response, int status, const std::string &code, const std::string &message);

//! X-OF-Data-Version and X-OF-Catalog-Version, which every /v1 response carries.
void SetVersionHeaders(httplib::Response &response, duckdb::Connection &connection);

void RegisterHealthRoutes(httplib::Server &server, const RouteContext &context);

} // namespace ofexcel
