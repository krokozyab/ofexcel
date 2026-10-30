#pragma once

// What a route file needs: httplib, the database, and the helpers that make
// every response look the same. Internal to src/server; not an installed header.
//
// Adding a route group: create src/server/routes_<group>.cpp with a
// Register<Group>Routes function, declare it below, call it from
// RegisterRoutes in http_server.cpp, add the file to CMakeLists.txt.

#include "duckdb.hpp"
#include "ofexcel/api_error.hpp"
#include "ofexcel/http_server.hpp"

#include <httplib.h>

#include <functional>
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
//! so it is written in English (CONTRACTS.md §8.1).
void SendError(httplib::Response &response, int status, const std::string &code, const std::string &message);

//! An ApiError as its status and error body, details included.
void SendApiError(httplib::Response &response, const ApiError &error);

//! X-OF-Data-Version and X-OF-Catalog-Version, which every /v1 response carries.
void SetVersionHeaders(httplib::Response &response, duckdb::Connection &connection);

using ConnectionHandler = std::function<void(const httplib::Request &, httplib::Response &, duckdb::Connection &)>;

//! A /v1 handler with the request's own connection. Sets the version headers
//! after `handler` ran, so a route that changes the catalog reports the new
//! version, and turns an ApiError into its error body.
httplib::Server::Handler WithConnection(const RouteContext &context, ConnectionHandler handler);

//! The body as a JSON object, or ApiError E_BAD_REQUEST (400).
JsonDocument ParseBody(const httplib::Request &request);

//! The string member `key`, or ApiError E_BAD_REQUEST (422) naming it.
std::string RequiredString(const JsonValue &body, const std::string &key);

//! Where the local profile serves the add-in's files (src/server/routes_addin.cpp).
constexpr const char *ADDIN_PREFIX = "/addin/";
constexpr size_t ADDIN_PREFIX_LENGTH = 7;

void RegisterHealthRoutes(httplib::Server &server, const RouteContext &context);
//! /v1/status, /v1/connections*, /v1/auth/* (src/server/routes_connections.cpp).
void RegisterConnectionRoutes(httplib::Server &server, const RouteContext &context);
//! /v1/queries*, /v1/workbooks/{id}/queries, /v1/trust (src/server/routes_queries.cpp).
void RegisterQueryRoutes(httplib::Server &server, const RouteContext &context);
//! /v1/runs*, /v1/datasets*, /v1/preview (src/server/routes_runs.cpp).
void RegisterRunRoutes(httplib::Server &server, const RouteContext &context);
//! /v1/imports* (src/server/routes_imports.cpp).
void RegisterImportRoutes(httplib::Server &server, const RouteContext &context);
//! Local profile only: GET /addin/*, with the session token in every HTML page.
void RegisterAddinRoutes(httplib::Server &server, const RouteContext &context);
//! Phase 0 only; registered when ServerOptions::probe_routes is set.
void RegisterProbeRoutes(httplib::Server &server, const RouteContext &context);

} // namespace ofexcel
