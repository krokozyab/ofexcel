#include "ofexcel/sql_functions.hpp"

#include "duckdb/function/table_function.hpp"
#include "ofexcel/api_error.hpp"
#include "ofexcel/connections.hpp"
#include "ofexcel/http_server.hpp"
#include "ofexcel/local_validator.hpp"
#include "ofexcel/paths.hpp"
#include "ofexcel/query_registry.hpp"
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

//! ofexcel_tls_dir, or QEXCEL_HOME/tls when it is empty.
std::string TlsDirSetting(ClientContext &context) {
	const auto configured = StringSetting(context, "ofexcel_tls_dir");
	if (!configured.empty()) {
		return configured;
	}
	try {
		return DefaultTlsDir();
	} catch (const std::exception &error) {
		throw duckdb::InvalidInputException("E_BAD_REQUEST: %s; задайте SET ofexcel_tls_dir", error.what());
	}
}

//! ofexcel_projects_dir, or ~/QExcel/projects when it is empty.
std::string ProjectsDirSetting(ClientContext &context) {
	const auto configured = StringSetting(context, "ofexcel_projects_dir");
	if (!configured.empty()) {
		return configured;
	}
	try {
		return DefaultProjectsDir();
	} catch (const std::exception &error) {
		throw duckdb::InvalidInputException("E_BAD_REQUEST: %s; задайте SET ofexcel_projects_dir", error.what());
	}
}

//! Reads ofexcel_profile, ofexcel_hosted_origin, ofexcel_port, ofexcel_tls_dir,
//! ofexcel_addin_dir and ofexcel_projects_dir into the options a server starts
//! with. Every refusal names the setting to fix.
ServerOptions OptionsFromSettings(ClientContext &context, int64_t port_override) {
	ServerOptions options;

	const auto profile = StringSetting(context, "ofexcel_profile");
	if (profile == "local") {
		// Same origin: the daemon serves the add-in, and the session token in
		// its pages guards the API instead of an Origin.
		options.guard.profile = Profile::LOCAL;
		options.addin_dir = StringSetting(context, "ofexcel_addin_dir");
	} else if (profile == "hosted") {
		options.guard.profile = Profile::HOSTED;
		const auto configured_origin = StringSetting(context, "ofexcel_hosted_origin");
		options.guard.hosted_origin = NormalizeHostedOrigin(configured_origin);
		if (options.guard.hosted_origin.empty()) {
			throw duckdb::InvalidInputException(
			    "E_BAD_REQUEST: ofexcel_hosted_origin должен быть origin вида https://host[:port] без пути, а сейчас "
			    "'%s'. Например: SET ofexcel_hosted_origin = 'https://intranet.example.com'",
			    configured_origin);
		}
	} else {
		throw duckdb::InvalidInputException("E_BAD_REQUEST: ofexcel_profile должен быть 'hosted' или 'local', а не '%s'",
		                                    profile);
	}

	const auto port = port_override >= 0 ? port_override : IntegerSetting(context, "ofexcel_port", DEFAULT_PORT);
	if (port < 0 || port > 65535) {
		throw duckdb::InvalidInputException("E_BAD_REQUEST: порт должен быть от 0 до 65535, а не %lld",
		                                    static_cast<long long>(port));
	}
	options.guard.port = static_cast<int>(port);
	options.tls_dir = TlsDirSetting(context);
	options.projects_dir = ProjectsDirSetting(context);
	Value probe;
	options.probe_routes = context.TryGetCurrentSetting("ofexcel_probe_routes", probe) && !probe.IsNull() && probe.GetValue<bool>();
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
	std::string tls_dir;
	try {
		tls_dir = TlsDirSetting(context);
	} catch (const std::exception &) {
		// Status reports what it can; ofexcel_start names the problem.
	}
	add("tls_dir", tls_dir);
	return std::move(state);
}

// --- ofexcel_validate --------------------------------------------------------

struct ValidateBindData : public duckdb::TableFunctionData {
	std::string sql;
	std::vector<std::string> depends;
	std::vector<std::string> params;
};

std::vector<std::string> StringList(TableFunctionBindInput &input, const char *name) {
	std::vector<std::string> strings;
	const auto entry = input.named_parameters.find(name);
	if (entry == input.named_parameters.end() || entry->second.IsNull()) {
		return strings;
	}
	for (const auto &child : duckdb::ListValue::GetChildren(entry->second)) {
		if (child.IsNull()) {
			throw duckdb::InvalidInputException("E_BAD_REQUEST: ofexcel_validate: в %s не может быть NULL", name);
		}
		strings.push_back(child.ToString());
	}
	return strings;
}

duckdb::unique_ptr<FunctionData> ValidateBind(ClientContext &, TableFunctionBindInput &input,
                                              duckdb::vector<LogicalType> &return_types,
                                              duckdb::vector<Identifier> &names) {
	if (input.inputs.empty() || input.inputs[0].IsNull()) {
		throw duckdb::InvalidInputException("E_BAD_REQUEST: ofexcel_validate: текст SQL не может быть NULL");
	}
	auto bind_data = duckdb::make_uniq<ValidateBindData>();
	bind_data->sql = input.inputs[0].ToString();
	bind_data->depends = StringList(input, "depends");
	bind_data->params = StringList(input, "params");
	names = {"ok", "rule", "message"};
	return_types = {LogicalType::BOOLEAN, LogicalType::VARCHAR, LogicalType::VARCHAR};
	return std::move(bind_data);
}

//! One row: ok, and for a refusal the rule (V1–V8, NULL if the text does not
//! parse) and "E_VALIDATION: …" or "E_SQL_COMPILE: …".
duckdb::unique_ptr<GlobalTableFunctionState> ValidateInit(ClientContext &, TableFunctionInitInput &input) {
	const auto &bind_data = input.bind_data->Cast<ValidateBindData>();
	const auto verdict =
	    ValidateLocalSql(bind_data.sql, LocalSqlScope::FromLists(bind_data.depends, bind_data.params));
	auto state = duckdb::make_uniq<RowsState>();
	if (verdict.ok) {
		state->rows.push_back({Value::BOOLEAN(true), Value(LogicalType::VARCHAR), Value(LogicalType::VARCHAR)});
	} else {
		state->rows.push_back({Value::BOOLEAN(false), verdict.rule.empty() ? Value(LogicalType::VARCHAR) : Value(verdict.rule),
		                       Value(verdict.code + ": " + verdict.message)});
	}
	return std::move(state);
}

// --- ofexcel_set_environment -------------------------------------------------

struct SetEnvironmentBindData : public duckdb::TableFunctionData {
	std::string name;
	std::string environment;
};

duckdb::unique_ptr<FunctionData> SetEnvironmentBind(ClientContext &, TableFunctionBindInput &input,
                                                    duckdb::vector<LogicalType> &return_types,
                                                    duckdb::vector<Identifier> &names) {
	if (input.inputs.size() != 2 || input.inputs[0].IsNull() || input.inputs[1].IsNull()) {
		throw duckdb::InvalidInputException(
		    "E_BAD_REQUEST: ofexcel_set_environment(name, environment): имя и окружение не могут быть NULL");
	}
	auto bind_data = duckdb::make_uniq<SetEnvironmentBindData>();
	bind_data->name = input.inputs[0].ToString();
	bind_data->environment = input.inputs[1].ToString();
	names = {"name", "environment"};
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR};
	return std::move(bind_data);
}

//! Writes meta.connections on a connection of its own: the calling one is busy
//! running this very query.
duckdb::unique_ptr<GlobalTableFunctionState> SetEnvironmentInit(ClientContext &context, TableFunctionInitInput &input) {
	const auto &bind_data = input.bind_data->Cast<SetEnvironmentBindData>();
	duckdb::Connection connection(*context.db);
	FusionConnection labelled;
	try {
		labelled = SetConnectionEnvironment(connection, bind_data.name, bind_data.environment);
	} catch (const ApiError &error) {
		throw duckdb::InvalidInputException("%s: %s", error.code, error.what());
	}
	auto state = duckdb::make_uniq<RowsState>();
	state->rows.push_back({Value(labelled.name), Value(labelled.environment)});
	return std::move(state);
}

// --- ofexcel_queries ---------------------------------------------------------

duckdb::unique_ptr<FunctionData> QueriesBind(ClientContext &, TableFunctionBindInput &,
                                             duckdb::vector<LogicalType> &return_types,
                                             duckdb::vector<Identifier> &names) {
	names = {"query_id", "definition_hash", "version", "engine", "source_kind", "source_ref", "trusted"};
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::INTEGER, LogicalType::VARCHAR,
	                LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::BOOLEAN};
	return duckdb::make_uniq<duckdb::TableFunctionData>();
}

//! Reads on a connection of its own, as ofexcel_set_environment writes.
duckdb::unique_ptr<GlobalTableFunctionState> QueriesInit(ClientContext &context, TableFunctionInitInput &) {
	duckdb::Connection connection(*context.db);
	auto state = duckdb::make_uniq<RowsState>();
	for (const auto &definition : AllDefinitions(connection)) {
		state->rows.push_back({Value(definition.query_id), Value(definition.definition_hash),
		                       Value::INTEGER(definition.version), Value(definition.engine),
		                       Value(definition.source_kind), Value(definition.source_ref),
		                       Value::BOOLEAN(definition.trusted)});
	}
	return std::move(state);
}

// --- ofexcel_reload ----------------------------------------------------------

struct ReloadBindData : public duckdb::TableFunctionData {
	std::string projects_dir;
};

duckdb::unique_ptr<FunctionData> ReloadBind(ClientContext &context, TableFunctionBindInput &,
                                            duckdb::vector<LogicalType> &return_types,
                                            duckdb::vector<Identifier> &names) {
	auto bind_data = duckdb::make_uniq<ReloadBindData>();
	bind_data->projects_dir = ProjectsDirSetting(context);
	names = {"source", "query_id", "ok", "message"};
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::BOOLEAN, LogicalType::VARCHAR};
	return std::move(bind_data);
}

//! One row per project file read; query_id and message NULL where empty.
duckdb::unique_ptr<GlobalTableFunctionState> ReloadInit(ClientContext &context, TableFunctionInitInput &input) {
	const auto &bind_data = input.bind_data->Cast<ReloadBindData>();
	duckdb::Connection connection(*context.db);
	auto state = duckdb::make_uniq<RowsState>();
	for (const auto &entry : ReloadProjects(connection, bind_data.projects_dir)) {
		state->rows.push_back({Value(entry.source),
		                       entry.query_id.empty() ? Value(LogicalType::VARCHAR) : Value(entry.query_id),
		                       Value::BOOLEAN(entry.ok),
		                       entry.message.empty() ? Value(LogicalType::VARCHAR) : Value(entry.message)});
	}
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
	// Global by default: they configure the daemon, and a plain SET in init.sql
	// must reach the connections HTTP requests open later. DuckDB 2.0 makes an
	// extension option session-scoped unless told otherwise.
	const auto add = [&](const char *name, const char *description, LogicalType type, Value default_value) {
		config.AddExtensionOption(name, description, std::move(type), default_value, nullptr, duckdb::SetScope::GLOBAL);
	};
	add("ofexcel_port", "Port ofexceld listens on at 127.0.0.1; 0 picks a free one.", LogicalType::INTEGER,
	    Value::INTEGER(DEFAULT_PORT));
	add("ofexcel_profile",
	    "How the Excel add-in is served: 'hosted' (HTTPS host, API over https://127.0.0.1) or "
	    "'local' (served by ofexceld over https://localhost).",
	    LogicalType::VARCHAR, Value("hosted"));
	add("ofexcel_tls_dir",
	    "Directory with the local CA and the server certificate (ofexceld install-cert); empty means QEXCEL_HOME/tls.",
	    LogicalType::VARCHAR, Value(""));
	add("ofexcel_hosted_origin", "The one Origin the hosted add-in is served from, e.g. https://intranet.example.com.",
	    LogicalType::VARCHAR, Value(""));
	add("ofexcel_addin_dir",
	    "Local profile: directory /addin/ is served from; empty means the files built into ofexceld.",
	    LogicalType::VARCHAR, Value(""));
	add("ofexcel_projects_dir",
	    "Directory of query projects, <project>/queries/*.sql; empty means ~/QExcel/projects.", LogicalType::VARCHAR,
	    Value(""));
	add("ofexcel_probe_routes", "Phase 0 only: serve /v1/probe/rows for the add-in probe. Off by default.",
	    LogicalType::BOOLEAN, Value::BOOLEAN(false));
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

	TableFunction validate("ofexcel_validate", {LogicalType::VARCHAR}, ScanRows, ValidateBind, ValidateInit);
	AddNamedParameter(validate, "depends", LogicalType::LIST(LogicalType::VARCHAR));
	AddNamedParameter(validate, "params", LogicalType::LIST(LogicalType::VARCHAR));
	loader.RegisterFunction(validate);

	loader.RegisterFunction(TableFunction("ofexcel_set_environment", {LogicalType::VARCHAR, LogicalType::VARCHAR},
	                                      ScanRows, SetEnvironmentBind, SetEnvironmentInit));

	loader.RegisterFunction(RowsFunction("ofexcel_queries", QueriesBind, QueriesInit));
	loader.RegisterFunction(RowsFunction("ofexcel_reload", ReloadBind, ReloadInit));

	loader.RegisterFunction(RowsFunction("ofexcel_stop", StopBind, StopInit));
	loader.RegisterFunction(RowsFunction("ofexcel_status", StatusBind, StatusInit));
}

} // namespace ofexcel
