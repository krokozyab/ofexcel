#include "ofexcel/sql_functions.hpp"

#include "duckdb/function/table_function.hpp"
#include "ofexcel/http_server.hpp"
#include "ofexcel/version.hpp"

#include <string>
#include <vector>

namespace ofexcel {

using duckdb::ClientContext;
using duckdb::DataChunk;
using duckdb::ExpressionState;
using duckdb::FunctionData;
using duckdb::GlobalTableFunctionState;
using duckdb::Identifier;
using duckdb::idx_t;
using duckdb::LogicalType;
using duckdb::TableFunction;
using duckdb::TableFunctionBindInput;
using duckdb::TableFunctionInitInput;
using duckdb::TableFunctionInput;
using duckdb::Value;
using duckdb::Vector;

namespace {

constexpr int64_t DEFAULT_PORT = 47321;

// --- Settings --------------------------------------------------------------

std::string StringSetting(ClientContext &context, const char *name) {
	Value value;
	if (context.TryGetCurrentSetting(name, value) && !value.IsNull()) {
		return value.ToString();
	}
	return "";
}

int64_t IntegerSetting(ClientContext &context, const char *name, int64_t fallback) {
	Value value;
	if (context.TryGetCurrentSetting(name, value) && !value.IsNull()) {
		return value.GetValue<int64_t>();
	}
	return fallback;
}

//! Reads ofexcel_profile, ofexcel_hosted_origin and ofexcel_port into the
//! options a server starts with. Every refusal names the setting to fix.
ServerOptions OptionsFromSettings(ClientContext &context, int64_t port_override) {
	ServerOptions options;

	const auto profile = StringSetting(context, "ofexcel_profile");
	if (profile == "local") {
		// The local profile serves the add-in over https://localhost with a
		// session token embedded in the pages; neither exists yet.
		throw duckdb::NotImplementedException("E_UNSUPPORTED: профиль 'local' (HTTPS на localhost) ещё не реализован; "
		                                      "используйте SET ofexcel_profile = 'hosted'");
	}
	if (profile != "hosted") {
		throw duckdb::InvalidInputException("E_BAD_REQUEST: ofexcel_profile должен быть 'hosted' или 'local', а не '%s'",
		                                    profile);
	}
	options.guard.profile = Profile::HOSTED;

	const auto configured_origin = StringSetting(context, "ofexcel_hosted_origin");
	options.guard.hosted_origin = NormalizeHostedOrigin(configured_origin);
	if (options.guard.hosted_origin.empty()) {
		throw duckdb::InvalidInputException(
		    "E_BAD_REQUEST: ofexcel_hosted_origin должен быть origin вида https://host[:port] без пути, а сейчас '%s'. "
		    "Например: SET ofexcel_hosted_origin = 'https://intranet.example.com'",
		    configured_origin);
	}

	const auto port = port_override >= 0 ? port_override : IntegerSetting(context, "ofexcel_port", DEFAULT_PORT);
	if (port < 0 || port > 65535) {
		throw duckdb::InvalidInputException("E_BAD_REQUEST: порт должен быть от 0 до 65535, а не %lld",
		                                    static_cast<long long>(port));
	}
	options.guard.port = static_cast<int>(port);
	return options;
}

// --- A table function that returns rows computed once ------------------------
//
// The pattern every ofexcel_* table function follows: bind declares the columns
// and validates arguments, and the *action* happens in the global init, which
// runs when the query executes -- not when it is bound, which EXPLAIN and
// prepared statements also do.

struct RowsState : public GlobalTableFunctionState {
	std::vector<std::vector<Value>> rows;
	idx_t offset = 0;

	idx_t MaxThreads() const override {
		return 1;
	}
};

void ScanRows(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<RowsState>();
	const auto remaining = state.rows.size() - state.offset;
	const auto count = duckdb::MinValue<idx_t>(STANDARD_VECTOR_SIZE, remaining);
	output.SetChildCardinality(count);
	for (idx_t row = 0; row < count; row++) {
		const auto &values = state.rows[state.offset + row];
		for (idx_t column = 0; column < values.size(); column++) {
			output.data[column].SetValue(row, values[column]);
		}
	}
	state.offset += count;
}

struct PortBindData : public duckdb::TableFunctionData {
	int64_t port = -1; //!< -1: take ofexcel_port
};

int64_t PortParameter(TableFunctionBindInput &input) {
	const auto entry = input.named_parameters.find("port");
	if (entry == input.named_parameters.end() || entry->second.IsNull()) {
		return -1;
	}
	return entry->second.GetValue<int64_t>();
}

// --- ofexcel_start / ofexcel_serve ------------------------------------------

duckdb::unique_ptr<FunctionData> StartBind(ClientContext &, TableFunctionBindInput &input,
                                           duckdb::vector<LogicalType> &return_types,
                                           duckdb::vector<Identifier> &names) {
	auto bind_data = duckdb::make_uniq<PortBindData>();
	bind_data->port = PortParameter(input);
	names = {"url"};
	return_types = {LogicalType::VARCHAR};
	return std::move(bind_data);
}

duckdb::unique_ptr<GlobalTableFunctionState> StartInit(ClientContext &context, TableFunctionInitInput &input) {
	const auto &bind_data = input.bind_data->Cast<PortBindData>();
	auto options = OptionsFromSettings(context, bind_data.port);
	auto state = duckdb::make_uniq<RowsState>();
	state->rows.push_back({Value(HttpServer::Get().Start(context.db, options))});
	return std::move(state);
}

duckdb::unique_ptr<FunctionData> ServeBind(ClientContext &, TableFunctionBindInput &input,
                                           duckdb::vector<LogicalType> &return_types,
                                           duckdb::vector<Identifier> &names) {
	auto bind_data = duckdb::make_uniq<PortBindData>();
	bind_data->port = PortParameter(input);
	names = {"stopped"};
	return_types = {LogicalType::BOOLEAN};
	return std::move(bind_data);
}

//! Blocks the calling query until the server stops: ofexcel_stop() from
//! another connection, or SIGINT/SIGTERM in ofexceld.
duckdb::unique_ptr<GlobalTableFunctionState> ServeInit(ClientContext &context, TableFunctionInitInput &input) {
	const auto &bind_data = input.bind_data->Cast<PortBindData>();
	auto options = OptionsFromSettings(context, bind_data.port);
	auto &server = HttpServer::Get();
	server.Start(context.db, options);
	server.Wait();
	auto state = duckdb::make_uniq<RowsState>();
	state->rows.push_back({Value::BOOLEAN(true)});
	return std::move(state);
}

// --- ofexcel_stop ------------------------------------------------------------

duckdb::unique_ptr<FunctionData> StopBind(ClientContext &, TableFunctionBindInput &,
                                          duckdb::vector<LogicalType> &return_types,
                                          duckdb::vector<Identifier> &names) {
	names = {"stopped"};
	return_types = {LogicalType::BOOLEAN};
	return duckdb::make_uniq<duckdb::TableFunctionData>();
}

duckdb::unique_ptr<GlobalTableFunctionState> StopInit(ClientContext &, TableFunctionInitInput &) {
	auto state = duckdb::make_uniq<RowsState>();
	state->rows.push_back({Value::BOOLEAN(HttpServer::Get().Stop())});
	return std::move(state);
}

// --- ofexcel_status ----------------------------------------------------------

duckdb::unique_ptr<FunctionData> StatusBind(ClientContext &, TableFunctionBindInput &,
                                            duckdb::vector<LogicalType> &return_types,
                                            duckdb::vector<Identifier> &names) {
	names = {"key", "value"};
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR};
	return duckdb::make_uniq<duckdb::TableFunctionData>();
}

duckdb::unique_ptr<GlobalTableFunctionState> StatusInit(ClientContext &context, TableFunctionInitInput &) {
	auto &server = HttpServer::Get();
	auto state = duckdb::make_uniq<RowsState>();
	const auto add = [&](const char *key, const std::string &value) {
		state->rows.push_back({Value(key), Value(value)});
	};
	add("version", VersionWithBuildStamp());
	add("api_version", API_VERSION);
	add("server_running", server.IsRunning() ? "true" : "false");
	add("url", server.Url());
	add("profile", StringSetting(context, "ofexcel_profile"));
	add("hosted_origin", StringSetting(context, "ofexcel_hosted_origin"));
	return std::move(state);
}

// --- ofexcel_version ---------------------------------------------------------

void VersionFunction(DataChunk &, ExpressionState &, Vector &result) {
	result.SetVectorType(duckdb::VectorType::CONSTANT_VECTOR);
	duckdb::ConstantVector::GetData<duckdb::string_t>(result)[0] =
	    duckdb::StringVector::AddString(result, VersionWithBuildStamp());
}

TableFunction RowsFunction(const char *name, duckdb::table_function_bind_t bind,
                           duckdb::table_function_init_global_t init) {
	TableFunction function(name, {}, ScanRows, bind, init);
	return function;
}

//! DuckDB 2.0 declares named parameters on the signature as typed kwargs; the
//! 1.x `named_parameters` map is gone. The same helper ofquack uses.
void AddNamedParameter(TableFunction &function, const std::string &name, LogicalType type) {
	const auto declare = [&](duckdb::TypedKwargs &options) {
		options.Add(Identifier(name), std::move(type));
	};
	auto &signature = function.GetSignature();
	if (signature.GetTypedKwargs()) {
		signature.ExtendTypedKwargs(declare);
	} else {
		signature.WithTypedKwargs(Identifier("options"), declare);
	}
}

} // namespace

void RegisterSettings(duckdb::ExtensionLoader &loader) {
	auto &config = duckdb::DBConfig::GetConfig(loader.GetDatabaseInstance());
	config.AddExtensionOption("ofexcel_port", "Port ofexceld listens on at 127.0.0.1; 0 picks a free one.",
	                          LogicalType::INTEGER, Value::INTEGER(DEFAULT_PORT));
	config.AddExtensionOption("ofexcel_profile",
	                          "How the Excel add-in is served: 'hosted' (HTTPS host, API over http://127.0.0.1) or "
	                          "'local' (served by ofexceld over https://localhost).",
	                          LogicalType::VARCHAR, Value("hosted"));
	config.AddExtensionOption("ofexcel_hosted_origin",
	                          "The one Origin the hosted add-in is served from, e.g. https://intranet.example.com.",
	                          LogicalType::VARCHAR, Value(""));
}

void RegisterSqlFunctions(duckdb::ExtensionLoader &loader) {
	loader.RegisterFunction(
	    duckdb::ScalarFunction("ofexcel_version", {}, LogicalType::VARCHAR, VersionFunction));

	auto start = RowsFunction("ofexcel_start", StartBind, StartInit);
	AddNamedParameter(start, "port", LogicalType::INTEGER);
	loader.RegisterFunction(start);

	auto serve = RowsFunction("ofexcel_serve", ServeBind, ServeInit);
	AddNamedParameter(serve, "port", LogicalType::INTEGER);
	loader.RegisterFunction(serve);

	loader.RegisterFunction(RowsFunction("ofexcel_stop", StopBind, StopInit));
	loader.RegisterFunction(RowsFunction("ofexcel_status", StatusBind, StatusInit));
}

} // namespace ofexcel
