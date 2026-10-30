// Formulas over HTTP (CONTRACTS.md §10.3 /v1/batch, §8.3 /v1/catalog;
// TASKS.md C8). The evaluation itself is src/batch/formulas.cpp.

#include "routes.hpp"

#include "ofexcel/api_error.hpp"
#include "ofexcel/formulas.hpp"
#include "ofexcel/json_body.hpp"
#include "ofexcel/migrations.hpp"
#include "ofexcel/query_registry.hpp"

#include <string>
#include <vector>

namespace ofexcel {

namespace {

//! Calls one batch may hold.
constexpr size_t MAX_BATCH_CALLS = 10000;

ApiError BadRequest(const std::string &message) {
	return ApiError("E_BAD_REQUEST", 422, message);
}

//! An integer `i`, as the add-in numbers its calls.
int64_t CallNumber(const JsonValue &call, size_t position) {
	const auto value = call.Get("i");
	const auto text = value.Is(JsonKind::NUMBER) ? value.NumberText() : "";
	const bool integer = !text.empty() && text.size() <= 15 &&
	                     text.find_first_not_of("-0123456789") == std::string::npos;
	if (!integer) {
		throw BadRequest("calls[" + std::to_string(position) + "].i must be an integer");
	}
	return std::stoll(text);
}

void BatchRoute(const httplib::Request &request, httplib::Response &response, duckdb::Connection &connection) {
	const auto document = ParseBody(request);
	const auto body = document.Root();

	BatchContext context;
	context.workbook_id = request.get_header_value("X-OF-Workbook");
	if (!context.workbook_id.empty() && !IsUuidV4(context.workbook_id)) {
		throw BadRequest("X-OF-Workbook must be the workbook id, a lowercase UUID v4");
	}
	const auto connection_name = body.Get("connection");
	if (connection_name.Is(JsonKind::STRING)) {
		context.connection = connection_name.String();
	} else if (!connection_name.Is(JsonKind::MISSING) && !connection_name.Is(JsonKind::NULL_VALUE)) {
		throw BadRequest("connection must be a string");
	}
	const auto date_system = body.Get("date_system");
	if (!date_system.Is(JsonKind::MISSING)) {
		const auto text = date_system.Is(JsonKind::NUMBER) ? date_system.NumberText() : "";
		if (text != "1900" && text != "1904") {
			throw BadRequest("date_system must be 1900 or 1904");
		}
		context.date1904 = text == "1904";
	}

	const auto listed = body.Get("calls");
	if (!listed.Is(JsonKind::ARRAY) || listed.Size() > MAX_BATCH_CALLS) {
		throw BadRequest("calls must be an array of at most " + std::to_string(MAX_BATCH_CALLS) + " calls");
	}
	std::vector<FormulaCall> calls;
	for (size_t n = 0; n < listed.Size(); n++) {
		const auto item = listed.At(n);
		const auto name = "calls[" + std::to_string(n) + "]";
		if (!item.Is(JsonKind::OBJECT)) {
			throw BadRequest(name + " must be an object {i, kind, query, args}");
		}
		FormulaCall call;
		call.i = CallNumber(item, n);
		call.kind = RequiredString(item, "kind");
		call.query = RequiredString(item, "query");
		call.args = FormulaArgs(item.Get("args"), name);
		calls.push_back(std::move(call));
	}

	const auto results = EvaluateBatch(*connection.context->db, context, calls);
	const auto state = ReadMetaState(connection);
	JsonObject out;
	out.Set("data_version", state.data_version).Set("catalog_version", state.catalog_version);
	auto list = out.SetArray("results");
	for (const auto &result : results) {
		list.AddObject(FormulaResultJson(result));
	}
	SendJson(response, 200, out.Serialize());
}

//! §8.3: the core's functions, as functions.json describes them.
void CatalogRoute(const httplib::Request &, httplib::Response &response, duckdb::Connection &connection) {
	const auto functions_json = JsonDocument::Parse(CoreFunctionsJson());
	const auto functions = functions_json.Root().Get("functions");
	JsonObject out;
	out.Set("catalog_version", ReadMetaState(connection).catalog_version);
	auto list = out.SetArray("functions");
	for (size_t f = 0; f < functions.Size(); f++) {
		const auto function = functions.At(f);
		JsonObject item;
		item.Set("id", function.Get("id").String())
		    .Set("name", function.Get("name").String())
		    .Set("description", function.Get("description").String());
		auto params = item.SetArray("params");
		const auto parameters = function.Get("parameters");
		for (size_t p = 0; p < parameters.Size(); p++) {
			JsonObject param;
			param.Set("name", parameters.At(p).Get("name").String())
			    .Set("type", parameters.At(p).Get("type").String());
			params.AddObject(param);
		}
		list.AddObject(item);
	}
	SendJson(response, 200, out.Serialize());
}

} // namespace

void RegisterFormulaRoutes(httplib::Server &server, const RouteContext &context) {
	server.Post("/v1/batch", WithConnection(context, BatchRoute));
	server.Get("/v1/catalog", WithConnection(context, CatalogRoute));
}

} // namespace ofexcel
