#include "ofexcel/http_server.hpp"

#include "ofexcel/json_body.hpp"
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
		return "Запрос пришёл не на 127.0.0.1 или localhost с портом ofexceld";
	case GuardVerdict::BAD_ORIGIN:
		return "Источник запроса (Origin) не разрешён для ofexceld";
	case GuardVerdict::BAD_SESSION:
		return "Нет или неверный токен сессии ofexceld";
	default:
		return "";
	}
}

//! Applies the guard before routing, and the CORS headers an allowed hosted
//! request needs. A refused request gets no CORS headers at all, so the browser
//! does not even show the page the error body (CONTRACTS.md §8.1).
void InstallGuard(httplib::Server &server, const GuardConfig &guard) {
	server.set_pre_routing_handler([guard](const httplib::Request &request, httplib::Response &response) {
		const auto headers = HeadersOf(request);
		const bool preflight = request.method == "OPTIONS";
		const auto verdict = preflight ? CheckPreflight(guard, headers) : CheckApiRequest(guard, headers);
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
			SendError(response, 404, "E_NOT_FOUND", "Нет такого маршрута");
		} else if (response.status == 413) {
			SendError(response, 413, "E_BAD_REQUEST", "Тело запроса слишком большое");
		} else {
			SendError(response, response.status, "E_BAD_REQUEST", "Запрос не обработан");
		}
	});
	server.set_exception_handler([](const httplib::Request &, httplib::Response &response, std::exception_ptr error) {
		std::string message = "Внутренняя ошибка ofexceld";
		try {
			std::rethrow_exception(error);
		} catch (const std::exception &exception) {
			message += ": " + duckdb::ErrorData(exception).Message();
		} catch (...) {
		}
		SendError(response, 500, "E_INTERNAL", message);
	});
}

void RegisterRoutes(httplib::Server &server, const RouteContext &context) {
	RegisterHealthRoutes(server, context);
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

void SetVersionHeaders(httplib::Response &response, duckdb::Connection &connection) {
	const auto state = ReadMetaState(connection);
	response.set_header("X-OF-Data-Version", std::to_string(state.data_version));
	response.set_header("X-OF-Catalog-Version", state.catalog_version);
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
		throw duckdb::InvalidInputException("E_CONFLICT: сервер ofexceld уже запущен на %s", state->url);
	}

	// Migrate before accepting requests, so no route ever sees a half-built schema.
	{
		duckdb::Connection connection(*db);
		RunMigrations(connection);
	}

	auto server = std::make_unique<httplib::Server>();
	server->set_payload_max_length(MAX_PAYLOAD_BYTES);

	const std::string bind_address = "127.0.0.1";
	int port = options.guard.port;
	if (port == 0) {
		port = server->bind_to_any_port(bind_address);
		if (port < 0) {
			throw duckdb::IOException("E_CONFLICT: не удалось занять свободный порт на %s", bind_address);
		}
	} else if (!server->bind_to_port(bind_address, port)) {
		throw duckdb::IOException("E_CONFLICT: порт %d на %s занят или недоступен", port, bind_address);
	}
	options.guard.port = port;

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
	state->url = "http://" + bind_address + ":" + std::to_string(port);
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
