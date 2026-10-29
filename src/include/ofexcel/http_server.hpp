#pragma once

// The daemon's HTTP server: one per process, bound to 127.0.0.1.
//
// httplib stays out of this header. Routes live in src/server/routes_*.cpp and
// see it through src/server/routes.hpp.

#include "duckdb.hpp"
#include "ofexcel/request_guard.hpp"

#include <memory>
#include <string>

namespace ofexcel {

struct ServerOptions {
	//! Profile, origins and session token. `guard.port` is the port to bind; 0
	//! picks a free one, and the guard then checks against the port bound.
	GuardConfig guard;
};

class HttpServer {
public:
	//! The process-wide server.
	static HttpServer &Get();

	//! Binds 127.0.0.1 and serves on background threads. Returns the base URL.
	//! Throws duckdb::InvalidInputException if a server is already running and
	//! duckdb::IOException if the port cannot be bound.
	std::string Start(duckdb::shared_ptr<duckdb::DatabaseInstance> db, ServerOptions options);

	//! Stops serving and joins the server thread. Returns false if nothing was
	//! running. Safe to call from any thread, and more than once.
	bool Stop();

	bool IsRunning() const;
	//! "" when not running.
	std::string Url() const;

	//! Blocks until Stop() is called or a stop is requested, then stops.
	void Wait();

	//! Async-signal-safe: sets a flag that Wait() polls. For SIGINT/SIGTERM.
	static void RequestStop();

	~HttpServer();
	HttpServer(const HttpServer &) = delete;
	HttpServer &operator=(const HttpServer &) = delete;

private:
	HttpServer();
	struct State;
	std::unique_ptr<State> state;
};

} // namespace ofexcel
