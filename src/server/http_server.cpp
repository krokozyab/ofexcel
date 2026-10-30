#include "ofexcel/http_server.hpp"

#include "ofexcel/api_error.hpp"
#include "ofexcel/json_body.hpp"
#include "ofexcel/local_ca.hpp"
#include "ofexcel/migrations.hpp"
#include "routes.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace ofexcel {

namespace {

//! Set from a signal handler, so nothing but a lock-free atomic may be touched.
std::atomic<bool> stop_requested {false};

//! Bodies above this are refused before a handler sees them. Imports arrive in
//! chunks (CONTRACTS.md §8.3), so no legitimate request comes close.
constexpr size_t MAX_PAYLOAD_BYTES = 16 * 1024 * 1024;

RequestHeaders HeadersOf(const httplib::Request &request) {
	RequestHeaders headers;
	headers.host = request.get_header_value("Host");
	headers.has_origin = request.has_header("Origin");
	headers.origin = request.get_header_value("Origin");
	headers.has_session = request.has_header("X-OF-Session");
	headers.session = request.get_header_value("X-OF-Session");
	return headers;
}

int StatusOf(GuardVerdict verdict) {
	return verdict == GuardVerdict::BAD_SESSION ? 401 : 403;
}

const char *MessageOf(GuardVerdict verdict) {
	switch (verdict) {
	case GuardVerdict::BAD_HOST:
		return "The request was not sent to 127.0.0.1 or localhost on the ofexceld port";
	case GuardVerdict::BAD_ORIGIN:
		return "The request origin (Origin) is not allowed by ofexceld";
	case GuardVerdict::BAD_SESSION:
		return "The ofexceld session token is missing or invalid";
	default:
		return "";
	}
}

bool IsAddinPath(const std::string &path) {
	return path.compare(0, ADDIN_PREFIX_LENGTH, ADDIN_PREFIX) == 0;
}

//! Applies the guard before routing, and the CORS headers an allowed hosted
//! request needs. A refused request gets no CORS headers at all, so the browser
//! does not even show the page the error body (CONTRACTS.md §8.1).
void InstallGuard(httplib::Server &server, const GuardConfig &guard) {
	server.set_pre_routing_handler([guard](const httplib::Request &request, httplib::Response &response) {
		const auto headers = HeadersOf(request);
		const bool preflight = request.method == "OPTIONS";
		// The local profile's own pages hand the session token out, so only Host
		// guards them (CheckStaticRequest). Everything else is the API.
		const bool add_in_file = guard.profile == Profile::LOCAL && !preflight && IsAddinPath(request.path);
		const auto verdict = add_in_file ? CheckStaticRequest(guard, headers)
		                     : preflight ? CheckPreflight(guard, headers)
		                                 : CheckApiRequest(guard, headers);
		if (verdict != GuardVerdict::ALLOW) {
			SendError(response, StatusOf(verdict), VerdictCode(verdict), MessageOf(verdict));
			return httplib::Server::HandlerResponse::Handled;
		}
		if (guard.profile == Profile::HOSTED) {
			response.set_header("Access-Control-Allow-Origin", guard.hosted_origin);
			response.set_header("Vary", "Origin");
		}
		return httplib::Server::HandlerResponse::Unhandled;
	});

	server.Options(R"(/v1/.*)", [](const httplib::Request &, httplib::Response &response) {
		response.set_header("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE");
		response.set_header("Access-Control-Allow-Headers", "Content-Type, X-OF-Workbook, X-OF-Session");
		response.set_header("Access-Control-Allow-Private-Network", "true");
		response.set_header("Access-Control-Max-Age", "600");
		response.status = 204;
	});
}

//! JSON bodies for what no route answered, and for exceptions a handler let out.
void InstallFallbacks(httplib::Server &server) {
	server.set_error_handler([](const httplib::Request &, httplib::Response &response) {
		if (!response.body.empty()) {
			return;
		}
		if (response.status == 404) {
			SendError(response, 404, "E_NOT_FOUND", "No such route");
		} else if (response.status == 413) {
			SendError(response, 413, "E_BAD_REQUEST", "The request body is too large");
		} else {
			SendError(response, response.status, "E_BAD_REQUEST", "The request was not handled");
		}
	});
	server.set_exception_handler([](const httplib::Request &, httplib::Response &response, std::exception_ptr error) {
		std::string message = "ofexceld internal error";
		try {
			std::rethrow_exception(error);
		} catch (const ApiError &api_error) {
			// A route's own refusal, with its §14 code and status.
			SendApiError(response, api_error);
			return;
		} catch (const std::exception &exception) {
			message += ": " + duckdb::ErrorData(exception).Message();
		} catch (...) {
		}
		SendError(response, 500, "E_INTERNAL", message);
	});
}

//! An HTTPS server with the certificate from `tls_dir`, reissued first if it is
//! about to expire. TlsError becomes the DuckDB exception CONTRACTS.md §14 maps.
std::unique_ptr<httplib::SSLServer> CreateTlsServer(const std::string &tls_dir) {
	ServerPem pem;
	try {
		pem = LoadServerCertificate(tls_dir);
	} catch (const TlsError &error) {
		if (error.kind == TlsError::Kind::MISSING) {
			throw duckdb::NotImplementedException("E_UNSUPPORTED: %s", error.what());
		}
		throw duckdb::IOException("E_INTERNAL: %s", error.what());
	}
	httplib::SSLServer::PemMemory memory {};
	memory.cert_pem = pem.cert_pem.c_str();
	memory.cert_pem_len = pem.cert_pem.size();
	memory.key_pem = pem.key_pem.c_str();
	memory.key_pem_len = pem.key_pem.size();
	auto server = std::make_unique<httplib::SSLServer>(memory);
	if (!server->is_valid()) {
		throw duckdb::IOException("E_INTERNAL: TLS rejected the server certificate in %s (code %d). Run ofexceld "
		                          "install-cert",
		                          tls_dir, server->ssl_last_error());
	}
	return server;
}

void RegisterRoutes(httplib::Server &server, const RouteContext &context) {
	RegisterHealthRoutes(server, context);
	RegisterConnectionRoutes(server, context);
	RegisterQueryRoutes(server, context);
	RegisterRunRoutes(server, context);
	RegisterImportRoutes(server, context);
	if (context.options.guard.profile == Profile::LOCAL) {
		RegisterAddinRoutes(server, context);
	}
	if (context.options.probe_routes) {
		RegisterProbeRoutes(server, context);
	}
}

} // namespace

duckdb::unique_ptr<duckdb::Connection> RouteContext::Connect() const {
	auto instance = db.lock();
	if (!instance) {
		return nullptr;
	}
	return duckdb::make_uniq<duckdb::Connection>(*instance);
}

void SendJson(httplib::Response &response, int status, const std::string &body) {
	response.status = status;
	response.set_content(body, "application/json; charset=utf-8");
}

void SendError(httplib::Response &response, int status, const std::string &code, const std::string &message) {
	SendJson(response, status, ErrorBody(code, message));
}

void SendApiError(httplib::Response &response, const ApiError &error) {
	if (error.details) {
		SendJson(response, error.status, ErrorBody(error.code, error.what(), *error.details));
	} else {
		SendError(response, error.status, error.code, error.what());
	}
}

void SetVersionHeaders(httplib::Response &response, duckdb::Connection &connection) {
	const auto state = ReadMetaState(connection);
	response.set_header("X-OF-Data-Version", std::to_string(state.data_version));
	response.set_header("X-OF-Catalog-Version", state.catalog_version);
}

httplib::Server::Handler WithConnection(const RouteContext &context, ConnectionHandler handler) {
	return [context, handler](const httplib::Request &request, httplib::Response &response) {
		auto connection = context.Connect();
		if (!connection) {
			SendError(response, 503, "E_INTERNAL", "The database is closed");
			return;
		}
		try {
			handler(request, response, *connection);
		} catch (const ApiError &error) {
			SendApiError(response, error);
		}
		SetVersionHeaders(response, *connection);
	};
}

JsonDocument ParseBody(const httplib::Request &request) {
	try {
		auto document = JsonDocument::Parse(request.body);
		if (document.Root().Is(JsonKind::OBJECT)) {
			return document;
		}
	} catch (const std::invalid_argument &) {
	}
	throw ApiError("E_BAD_REQUEST", 400, "The request body must be a JSON object");
}

std::string RequiredString(const JsonValue &body, const std::string &key) {
	const auto value = body.Get(key);
	if (!value.Is(JsonKind::STRING)) {
		throw ApiError("E_BAD_REQUEST", 422, "The request body has no string " + key);
	}
	return value.String();
}

struct HttpServer::State {
	mutable std::mutex mutex;
	std::condition_variable changed;
	std::unique_ptr<httplib::Server> server;
	std::thread thread;
	std::string url;
};

HttpServer::HttpServer() : state(std::make_unique<State>()) {
}

HttpServer::~HttpServer() {
	Stop();
}

HttpServer &HttpServer::Get() {
	static HttpServer instance;
	return instance;
}

std::string HttpServer::Start(duckdb::shared_ptr<duckdb::DatabaseInstance> db, ServerOptions options) {
	std::lock_guard<std::mutex> guard(state->mutex);
	if (state->server) {
		throw duckdb::InvalidInputException("E_CONFLICT: the ofexceld server is already running on %s", state->url);
	}

	// Certificates before migrations: a daemon that cannot serve HTTPS should
	// fail before it touches the database.
	std::unique_ptr<httplib::Server> server = CreateTlsServer(options.tls_dir);
	server->set_payload_max_length(MAX_PAYLOAD_BYTES);

	// Migrate before accepting requests, so no route ever sees a half-built schema.
	{
		duckdb::Connection connection(*db);
		RunMigrations(connection);
	}

	const std::string bind_address = "127.0.0.1";
	int port = options.guard.port;
	if (port == 0) {
		port = server->bind_to_any_port(bind_address);
		if (port < 0) {
			throw duckdb::IOException("E_CONFLICT: could not take a free port on %s", bind_address);
		}
	} else if (!server->bind_to_port(bind_address, port)) {
		throw duckdb::IOException("E_CONFLICT: port %d on %s is in use or unavailable", port, bind_address);
	}
	options.guard.port = port;
	if (options.guard.profile == Profile::LOCAL) {
		// New on every start: a page served by an earlier run holds a token
		// that no longer opens the API.
		try {
			options.guard.session_token = GenerateSessionToken();
		} catch (const TlsError &error) {
			throw duckdb::IOException("E_INTERNAL: %s", error.what());
		}
	}

	RouteContext context;
	context.db = db;
	context.options = options;
	InstallGuard(*server, options.guard);
	InstallFallbacks(*server);
	RegisterRoutes(*server, context);

	stop_requested.store(false);
	auto *raw = server.get();
	state->thread = std::thread([raw]() { raw->listen_after_bind(); });
	state->server = std::move(server);
	state->url = "https://" + bind_address + ":" + std::to_string(port);
	return state->url;
}

bool HttpServer::Stop() {
	std::unique_ptr<httplib::Server> server;
	std::thread thread;
	{
		std::lock_guard<std::mutex> guard(state->mutex);
		if (!state->server) {
			return false;
		}
		server = std::move(state->server);
		thread = std::move(state->thread);
		state->url.clear();
	}
	// Outside the lock: joining waits for in-flight handlers, and none of them
	// may need the lock to finish.
	server->stop();
	if (thread.joinable()) {
		thread.join();
	}
	state->changed.notify_all();
	return true;
}

bool HttpServer::IsRunning() const {
	std::lock_guard<std::mutex> guard(state->mutex);
	return state->server != nullptr;
}

std::string HttpServer::Url() const {
	std::lock_guard<std::mutex> guard(state->mutex);
	return state->url;
}

void HttpServer::Wait() {
	{
		std::unique_lock<std::mutex> lock(state->mutex);
		// Polled rather than signalled: the signal handler that sets the flag
		// may not touch a condition variable.
		while (state->server && !stop_requested.load()) {
			state->changed.wait_for(lock, std::chrono::milliseconds(200));
		}
	}
	Stop();
}

void HttpServer::RequestStop() {
	stop_requested.store(true);
}

} // namespace ofexcel
