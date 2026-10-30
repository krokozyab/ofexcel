#pragma once

// The daemon's HTTPS server: one per process, bound to 127.0.0.1, with a
// certificate from the local CA (DECISIONS.md Р6, src/security/local_ca.cpp).
//
// httplib stays out of this header. Routes live in src/server/routes_*.cpp and
// see it through src/server/routes.hpp.

#include "duckdb.hpp"
#include "ofexcel/request_guard.hpp"

#include <memory>
#include <string>

namespace ofexcel {

struct ServerOptions {
	//! Profile and origins. `guard.port` is the port to bind; 0 picks a free
	//! one, and the guard then checks against the port bound. In the local
	//! profile Start() sets `guard.session_token` to a fresh token.
	GuardConfig guard;
	//! The directory with the local CA and the server certificate.
	std::string tls_dir;
	//! Local profile: where /addin/* is served from; "" for the files built
	//! into the binary.
	std::string addin_dir;
	//! Where PUT /v1/queries/{id} writes project files: ofexcel_projects_dir, or
	//! ~/QExcel/projects when that is empty.
	std::string projects_dir;
	//! Phase 0 only: serve /v1/probe/* (src/server/routes_probe.cpp).
	bool probe_routes = false;
};

class HttpServer {
public:
	//! The process-wide server.
	static HttpServer &Get();

	//! Binds 127.0.0.1 and serves HTTPS on background threads. Returns the base
	//! URL. Reissues the server certificate first if it expires within
	//! SERVER_RENEW_DAYS. Throws duckdb::InvalidInputException if a server is
	//! already running, duckdb::NotImplementedException (E_UNSUPPORTED) if
	//! `tls_dir` has no CA, and duckdb::IOException if the certificate cannot be
	//! used or the port cannot be bound.
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
