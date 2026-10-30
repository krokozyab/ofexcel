// Importing a sheet's table over HTTP (CONTRACTS.md §8.3 /v1/imports, §12;
// TASKS.md C7): begin, batches of rows, commit. The import itself is
// src/meta/imports.cpp.

#include "routes.hpp"

#include "ofexcel/api_error.hpp"
#include "ofexcel/imports.hpp"
#include "ofexcel/json_body.hpp"
#include "ofexcel/param_values.hpp"

#include <string>
#include <vector>

namespace ofexcel {

namespace {

//! A cell as §3.2 encodes it; numbers keep their text, as parameters do. A
//! list goes on to be refused with the row's number in the whole import.
ParamInput Cell(const JsonValue &value, size_t row, size_t column) {
	switch (value.Kind()) {
	case JsonKind::MISSING:
	case JsonKind::NULL_VALUE:
		return ParamInput::Null();
	case JsonKind::BOOLEAN:
		return ParamInput::Boolean(value.Bool());
	case JsonKind::NUMBER:
		return ParamInput::Number(value.NumberText());
	case JsonKind::STRING:
		return ParamInput::String(value.String());
	case JsonKind::ARRAY: {
		std::vector<ParamInput> elements;
		for (size_t i = 0; i < value.Size(); i++) {
			elements.push_back(Cell(value.At(i), row, column));
		}
		return ParamInput::Array(std::move(elements));
	}
	case JsonKind::OBJECT:
		break;
	}
	throw ApiError("E_BAD_REQUEST", 422,
	               "Row " + std::to_string(row + 1) + " of this batch, value " + std::to_string(column + 1) +
	                   ": an object is not a cell value");
}

void BeginImportRoute(const httplib::Request &request, httplib::Response &response, duckdb::Connection &connection) {
	auto &store = ImportStore::Get();
	store.DropExpired(connection);
	const auto document = ParseBody(request);
	const auto body = document.Root();
	const auto name = RequiredString(body, "name");
	const auto workbook_id = RequiredString(body, "workbook_id");
	const auto source = RequiredString(body, "source");
	const auto listed = body.Get("columns");
	if (!listed.Is(JsonKind::ARRAY)) {
		throw ApiError("E_BAD_REQUEST", 422, "columns must be an array of {name, type}");
	}
	std::vector<ImportColumn> columns;
	for (size_t i = 0; i < listed.Size(); i++) {
		const auto column = listed.At(i);
		if (!column.Is(JsonKind::OBJECT)) {
			throw ApiError("E_BAD_REQUEST", 422, "columns must be an array of {name, type}");
		}
		columns.push_back({RequiredString(column, "name"), RequiredString(column, "type")});
	}
	const auto import_id = store.Begin(connection, name, workbook_id, source, columns);
	JsonObject out;
	out.Set("import_id", import_id);
	SendJson(response, 201, out.Serialize());
}

void ImportRowsRoute(const httplib::Request &request, httplib::Response &response, duckdb::Connection &connection) {
	auto &store = ImportStore::Get();
	store.DropExpired(connection);
	const auto document = ParseBody(request);
	const auto listed = document.Root().Get("rows");
	if (!listed.Is(JsonKind::ARRAY)) {
		throw ApiError("E_BAD_REQUEST", 422, "rows must be an array of rows, each an array of values");
	}
	std::vector<std::vector<ParamInput>> rows(listed.Size());
	for (size_t r = 0; r < listed.Size(); r++) {
		const auto row = listed.At(r);
		if (!row.Is(JsonKind::ARRAY)) {
			throw ApiError("E_BAD_REQUEST", 422, "rows must be an array of rows, each an array of values");
		}
		for (size_t c = 0; c < row.Size(); c++) {
			rows[r].push_back(Cell(row.At(c), r, c));
		}
	}
	store.AddRows(connection, request.matches[1].str(), rows);
	response.status = 204;
}

void CommitImportRoute(const httplib::Request &request, httplib::Response &response, duckdb::Connection &connection) {
	auto &store = ImportStore::Get();
	store.DropExpired(connection);
	const auto committed = store.Commit(connection, request.matches[1].str());
	JsonObject out;
	out.Set("name", committed.name).Set("rows", committed.rows);
	SendJson(response, 200, out.Serialize());
}

} // namespace

void RegisterImportRoutes(httplib::Server &server, const RouteContext &context) {
	server.Post("/v1/imports", WithConnection(context, BeginImportRoute));
	server.Post(R"(/v1/imports/([^/]+)/rows)", WithConnection(context, ImportRowsRoute));
	server.Post(R"(/v1/imports/([^/]+)/commit)", WithConnection(context, CommitImportRoute));
}

} // namespace ofexcel
