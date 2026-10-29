// Phase 0 only: GET /v1/probe/rows serves a table from the `probe` schema page by
// page, so the add-in probe (tools/addin-probe) can measure JSON transfer and
// writing to an Excel Table on real data. Not part of CONTRACTS.md; registered
// only when ofexcel_probe_routes is true, and to be removed once phase 0 is
// closed and the real /v1/runs/{id}/rows exists.

#include "routes.hpp"

#include "ofexcel/json_body.hpp"

#include <chrono>
#include <cmath>
#include <string>

namespace ofexcel {

namespace {

constexpr int64_t DEFAULT_LIMIT = 5000;
constexpr int64_t MAX_LIMIT = 100000;

bool IsIdentifier(const std::string &name) {
	if (name.empty() || name.size() > 63 || !(name[0] >= 'a' && name[0] <= 'z')) {
		return false;
	}
	for (const auto c : name) {
		if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {
			return false;
		}
	}
	return true;
}

//! A non-negative integer query parameter, or `fallback` when absent. -1 if malformed.
int64_t IntegerParam(const httplib::Request &request, const char *name, int64_t fallback) {
	if (!request.has_param(name)) {
		return fallback;
	}
	const auto text = request.get_param_value(name);
	if (text.empty() || text.size() > 12 || text.find_first_not_of("0123456789") != std::string::npos) {
		return -1;
	}
	return std::stoll(text);
}

//! The contract type a column is reported as (CONTRACTS.md §3.2), close enough
//! for a probe: the real mapping is the type contract's task.
std::string TypeName(const duckdb::LogicalType &type) {
	switch (type.id()) {
	case duckdb::LogicalTypeId::BOOLEAN:
		return "boolean";
	case duckdb::LogicalTypeId::TINYINT:
	case duckdb::LogicalTypeId::SMALLINT:
	case duckdb::LogicalTypeId::INTEGER:
	case duckdb::LogicalTypeId::BIGINT:
	case duckdb::LogicalTypeId::UTINYINT:
	case duckdb::LogicalTypeId::USMALLINT:
	case duckdb::LogicalTypeId::UINTEGER:
		return "integer";
	case duckdb::LogicalTypeId::DECIMAL:
		return "decimal(" + std::to_string(duckdb::DecimalType::GetWidth(type)) + "," +
		       std::to_string(duckdb::DecimalType::GetScale(type)) + ")";
	case duckdb::LogicalTypeId::FLOAT:
	case duckdb::LogicalTypeId::DOUBLE:
		return "number";
	case duckdb::LogicalTypeId::DATE:
		return "date";
	case duckdb::LogicalTypeId::TIMESTAMP:
		return "timestamp";
	default:
		return "string";
	}
}

void AddValue(JsonArrayRef &row, const duckdb::Value &value) {
	if (value.IsNull()) {
		row.AddNull();
		return;
	}
	switch (value.type().id()) {
	case duckdb::LogicalTypeId::BOOLEAN:
		row.AddBool(value.GetValue<bool>());
		return;
	case duckdb::LogicalTypeId::TINYINT:
	case duckdb::LogicalTypeId::SMALLINT:
	case duckdb::LogicalTypeId::INTEGER:
	case duckdb::LogicalTypeId::BIGINT:
	case duckdb::LogicalTypeId::UTINYINT:
	case duckdb::LogicalTypeId::USMALLINT:
	case duckdb::LogicalTypeId::UINTEGER:
		row.AddInt(value.GetValue<int64_t>());
		return;
	case duckdb::LogicalTypeId::FLOAT:
	case duckdb::LogicalTypeId::DOUBLE: {
		const auto number = value.GetValue<double>();
		if (std::isfinite(number)) {
			row.AddDouble(number);
		} else {
			row.AddNull();
		}
		return;
	}
	default:
		// DECIMAL travels as a string so JSON does not round it (CONTRACTS.md §3.2);
		// dates and timestamps as their ISO text.
		row.AddString(value.ToString());
		return;
	}
}

double MillisecondsSince(std::chrono::steady_clock::time_point start) {
	return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

} // namespace

void RegisterProbeRoutes(httplib::Server &server, const RouteContext &context) {
	server.Get("/v1/probe/rows", [context](const httplib::Request &request, httplib::Response &response) {
		const auto table = request.get_param_value("table");
		const auto offset = IntegerParam(request, "offset", 0);
		const auto limit = IntegerParam(request, "limit", DEFAULT_LIMIT);
		if (!IsIdentifier(table) || offset < 0 || limit < 1 || limit > MAX_LIMIT) {
			SendError(response, 400, "E_BAD_REQUEST",
			          "Нужны table (имя таблицы в схеме probe), offset ≥ 0 и limit от 1 до 100000");
			return;
		}
		auto connection = context.Connect();
		if (!connection) {
			SendError(response, 503, "E_INTERNAL", "База данных закрыта");
			return;
		}

		auto exists = connection->Prepare(
		    "SELECT count(*) FROM duckdb_tables() WHERE schema_name = 'probe' AND table_name = $1");
		auto found = exists->Execute(duckdb::Value(table));
		auto found_chunk = found->Fetch();
		if (!found_chunk || found_chunk->GetValue(0, 0).GetValue<int64_t>() == 0) {
			SendError(response, 404, "E_NOT_FOUND", "Нет таблицы probe." + table);
			return;
		}

		const auto query_started = std::chrono::steady_clock::now();
		// The name is checked by IsIdentifier above, so quoting it is enough here.
		const auto quoted = "probe.\"" + table + "\"";
		auto total_result = connection->Query("SELECT count(*) FROM " + quoted);
		const auto total = total_result->Fetch()->GetValue(0, 0).GetValue<int64_t>();
		auto page = connection->Prepare("SELECT * FROM " + quoted + " LIMIT $1 OFFSET $2");
		auto result = page->Execute(duckdb::Value::BIGINT(limit), duckdb::Value::BIGINT(offset));
		if (result->HasError()) {
			SendError(response, 500, "E_INTERNAL", result->GetError());
			return;
		}
		const auto query_ms = MillisecondsSince(query_started);

		const auto encode_started = std::chrono::steady_clock::now();
		JsonObject body;
		auto columns = body.SetArray("columns");
		const auto &types = result->GetTypes();
		const auto &names = result->GetNames();
		for (size_t column = 0; column < types.size(); column++) {
			JsonObject info;
			info.Set("name", names[column].GetIdentifierName()).Set("type", TypeName(types[column]));
			columns.AddObject(info);
		}
		auto rows = body.SetArray("rows");
		int64_t row_count = 0;
		while (auto chunk = result->Fetch()) {
			if (chunk->size() == 0) {
				break;
			}
			for (duckdb::idx_t row = 0; row < chunk->size(); row++) {
				auto values = rows.AddArray();
				for (duckdb::idx_t column = 0; column < types.size(); column++) {
					AddValue(values, chunk->GetValue(column, row));
				}
				row_count++;
			}
		}
		body.Set("offset", offset).Set("limit", limit).Set("total", total).Set("returned", row_count);
		const auto encode_ms = MillisecondsSince(encode_started);

		const auto serialize_started = std::chrono::steady_clock::now();
		auto serialized = body.Serialize();
		const auto serialize_ms = MillisecondsSince(serialize_started);

		response.set_header("X-OF-Probe-Query-Ms", std::to_string(query_ms));
		response.set_header("X-OF-Probe-Encode-Ms", std::to_string(encode_ms));
		response.set_header("X-OF-Probe-Serialize-Ms", std::to_string(serialize_ms));
		response.set_header("Access-Control-Expose-Headers",
		                    "X-OF-Probe-Query-Ms, X-OF-Probe-Encode-Ms, X-OF-Probe-Serialize-Ms, X-OF-Data-Version");
		SetVersionHeaders(response, *connection);
		SendJson(response, 200, serialized);
	});
}

} // namespace ofexcel
