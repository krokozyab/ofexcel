#include "ofexcel/sql_functions.hpp"

#include "duckdb/function/table_function.hpp"
#include "ofexcel/api_error.hpp"
#include "ofexcel/connections.hpp"
#include "ofexcel/datasets.hpp"
#include "ofexcel/db_util.hpp"
#include "ofexcel/formulas.hpp"
#include "ofexcel/http_server.hpp"
#include "ofexcel/local_validator.hpp"
#include "ofexcel/paths.hpp"
#include "ofexcel/query_registry.hpp"
#include "ofexcel/run_engine.hpp"
#include "ofexcel/version.hpp"

#include <map>
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
		throw duckdb::InvalidInputException("E_BAD_REQUEST: %s; use SET ofexcel_tls_dir", error.what());
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
		throw duckdb::InvalidInputException("E_BAD_REQUEST: %s; use SET ofexcel_projects_dir", error.what());
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
			    "E_BAD_REQUEST: ofexcel_hosted_origin must be an origin like https://host[:port] with no path, not "
			    "'%s'. For example: SET ofexcel_hosted_origin = 'https://intranet.example.com'",
			    configured_origin);
		}
	} else {
		throw duckdb::InvalidInputException("E_BAD_REQUEST: ofexcel_profile must be 'hosted' or 'local', not '%s'",
		                                    profile);
	}

	const auto port = port_override >= 0 ? port_override : IntegerSetting(context, "ofexcel_port", DEFAULT_PORT);
	if (port < 0 || port > 65535) {
		throw duckdb::InvalidInputException("E_BAD_REQUEST: the port must be from 0 to 65535, not %lld",
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
			throw duckdb::InvalidInputException("E_BAD_REQUEST: ofexcel_validate: %s cannot contain NULL", name);
		}
		strings.push_back(child.ToString());
	}
	return strings;
}

duckdb::unique_ptr<FunctionData> ValidateBind(ClientContext &, TableFunctionBindInput &input,
                                              duckdb::vector<LogicalType> &return_types,
                                              duckdb::vector<Identifier> &names) {
	if (input.inputs.empty() || input.inputs[0].IsNull()) {
		throw duckdb::InvalidInputException("E_BAD_REQUEST: ofexcel_validate: the SQL text cannot be NULL");
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
		    "E_BAD_REQUEST: ofexcel_set_environment(name, environment): name and environment cannot be NULL");
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

// --- ofexcel_run -------------------------------------------------------------

struct RunBindData : public duckdb::TableFunctionData {
	std::string query_id;
	std::string connection;
	std::map<std::string, ParamInput> params;
};

//! A SQL value as the add-in would have sent it in JSON: numbers as their text,
//! dates and timestamps as strings, lists as arrays.
ParamInput ParamInputFromValue(const Value &value) {
	if (value.IsNull()) {
		return ParamInput::Null();
	}
	const auto &type = value.type();
	switch (type.id()) {
	case duckdb::LogicalTypeId::BOOLEAN:
		return ParamInput::Boolean(value.GetValue<bool>());
	case duckdb::LogicalTypeId::LIST: {
		std::vector<ParamInput> elements;
		for (const auto &child : duckdb::ListValue::GetChildren(value)) {
			elements.push_back(ParamInputFromValue(child));
		}
		return ParamInput::Array(std::move(elements));
	}
	default:
		if (type.IsNumeric()) {
			return ParamInput::Number(value.ToString());
		}
		return ParamInput::String(value.ToString());
	}
}

duckdb::unique_ptr<FunctionData> RunBind(ClientContext &, TableFunctionBindInput &input,
                                         duckdb::vector<LogicalType> &return_types, duckdb::vector<Identifier> &names) {
	if (input.inputs.empty() || input.inputs[0].IsNull()) {
		throw duckdb::InvalidInputException("E_BAD_REQUEST: ofexcel_run: the query name cannot be NULL");
	}
	auto bind_data = duckdb::make_uniq<RunBindData>();
	bind_data->query_id = input.inputs[0].ToString();
	const auto connection = input.named_parameters.find("connection");
	if (connection != input.named_parameters.end() && !connection->second.IsNull()) {
		bind_data->connection = connection->second.ToString();
	}
	const auto params = input.named_parameters.find("params");
	if (params != input.named_parameters.end() && !params->second.IsNull()) {
		const auto &value = params->second;
		if (value.type().id() != duckdb::LogicalTypeId::STRUCT) {
			throw duckdb::InvalidInputException(
			    "E_BAD_REQUEST: ofexcel_run: params must be a struct, for example params := {'period': '2026-01'}");
		}
		const auto &children = duckdb::StructValue::GetChildren(value);
		for (idx_t i = 0; i < children.size(); i++) {
			bind_data->params[duckdb::StructType::GetChildName(value.type(), i).GetIdentifierName()] =
			    ParamInputFromValue(children[i]);
		}
	}
	names = {"run_id", "status", "rows_fetched", "truncated", "error_code", "error_message"};
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR,  LogicalType::BIGINT,
	                LogicalType::BOOLEAN, LogicalType::VARCHAR, LogicalType::VARCHAR};
	return std::move(bind_data);
}

//! Starts the run and waits for it. The query is resolved as §10.2 resolves it
//! without a workbook: projects, then examples.
duckdb::unique_ptr<GlobalTableFunctionState> RunInit(ClientContext &context, TableFunctionInitInput &input) {
	const auto &bind_data = input.bind_data->Cast<RunBindData>();
	duckdb::Connection connection(*context.db);
	RunRequest request;
	if (!FindQuery(connection, bind_data.query_id, QueryScope(), request.definition)) {
		throw duckdb::InvalidInputException("E_NOT_FOUND: no query %s", bind_data.query_id);
	}
	request.connection = bind_data.connection;
	request.params = bind_data.params;
	auto &engine = RunEngine::Get();
	RunStart started;
	try {
		started = engine.Start(*context.db, request);
	} catch (const ApiError &error) {
		throw duckdb::InvalidInputException("%s: %s", error.code, error.what());
	}
	engine.Wait(started.run_id);
	auto state = duckdb::make_uniq<RowsState>();
	for (auto &row : QueryRows(connection,
	                           "SELECT run_id, status, rows_fetched, truncated, error_code, error_message "
	                           "FROM meta.runs WHERE run_id = $1",
	                           {Value(started.run_id)})) {
		state->rows.push_back(std::move(row));
	}
	return std::move(state);
}

// --- ofexcel_runs / ofexcel_datasets -----------------------------------------

struct MetaTableBindData : public duckdb::TableFunctionData {
	std::string sql;
};

//! Every column of a meta.* table, as §6.2 declares it. The columns are read
//! from the table itself, so a migration that adds one needs no change here.
duckdb::unique_ptr<FunctionData> MetaTableBind(ClientContext &context, const std::string &sql,
                                               duckdb::vector<LogicalType> &return_types,
                                               duckdb::vector<Identifier> &names) {
	duckdb::Connection connection(*context.db);
	auto result = connection.Query("SELECT * FROM (" + sql + ") LIMIT 0");
	if (result->HasError()) {
		result->ThrowError("E_INTERNAL: ");
	}
	names = result->GetNames();
	return_types = result->GetTypes();
	auto bind_data = duckdb::make_uniq<MetaTableBindData>();
	bind_data->sql = sql;
	return std::move(bind_data);
}

duckdb::unique_ptr<GlobalTableFunctionState> MetaTableInit(ClientContext &context, TableFunctionInitInput &input) {
	const auto &bind_data = input.bind_data->Cast<MetaTableBindData>();
	duckdb::Connection connection(*context.db);
	auto state = duckdb::make_uniq<RowsState>();
	state->rows = QueryRows(connection, bind_data.sql);
	return std::move(state);
}

duckdb::unique_ptr<FunctionData> RunsBind(ClientContext &context, TableFunctionBindInput &,
                                          duckdb::vector<LogicalType> &return_types,
                                          duckdb::vector<Identifier> &names) {
	return MetaTableBind(context, "SELECT * FROM meta.runs ORDER BY started_at DESC, run_id", return_types, names);
}

duckdb::unique_ptr<FunctionData> DatasetsBind(ClientContext &context, TableFunctionBindInput &,
                                              duckdb::vector<LogicalType> &return_types,
                                              duckdb::vector<Identifier> &names) {
	return MetaTableBind(context, "SELECT * FROM meta.dataset_instances ORDER BY query_id, instance_key", return_types,
	                     names);
}

// --- ofexcel_version ---------------------------------------------------------

duckdb::unique_ptr<FunctionData> MissingBind(ClientContext &context, TableFunctionBindInput &,
                                             duckdb::vector<LogicalType> &return_types,
                                             duckdb::vector<Identifier> &names) {
	return MetaTableBind(context, "SELECT * FROM meta.missing ORDER BY workbook_id, query_id, instance_key",
	                     return_types, names);
}

// --- ofexcel_call --------------------------------------------------------------

//! One formula call the way /v1/batch evaluates it, without a workbook: the
//! query from projects and examples, @connection, the 1900 date system. The
//! result is the call's JSON from /v1/batch, without `i`.
void CallFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &db = *state.GetContext().db;
	for (idx_t row = 0; row < args.size(); row++) {
		const auto kind = args.GetValue(0, row);
		const auto query = args.GetValue(1, row);
		const auto arguments = args.GetValue(2, row);
		if (kind.IsNull() || query.IsNull()) {
			result.SetValue(row, Value(LogicalType::VARCHAR));
			continue;
		}
		FormulaCall call;
		call.kind = kind.ToString();
		call.query = query.ToString();
		try {
			if (!arguments.IsNull()) {
				const auto document = JsonDocument::Parse(arguments.ToString());
				call.args = FormulaArgs(document.Root(), "ofexcel_call");
			}
		} catch (const ApiError &error) {
			throw duckdb::InvalidInputException("%s: %s", error.code, error.what());
		} catch (const std::exception &error) {
			throw duckdb::InvalidInputException("E_BAD_REQUEST: ofexcel_call: args must be JSON: %s", error.what());
		}
		// Without "i": the call has no number here.
		result.SetValue(row, Value(FormulaResultJson(EvaluateBatch(db, BatchContext(), {call})[0], false).Serialize()));
	}
}

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
	add("ofexcel_run_timeout", "Seconds an Oracle run may take before it is interrupted with E_TIMEOUT.",
	    LogicalType::INTEGER, Value::INTEGER(static_cast<int32_t>(DEFAULT_RUN_TIMEOUT_SECONDS)));
	add("ofexcel_dataset_ttl_days",
	    "Dataset instances not read for this many days are dropped when ofexceld starts.", LogicalType::INTEGER,
	    Value::INTEGER(static_cast<int32_t>(DEFAULT_DATASET_TTL_DAYS)));
	add("ofexcel_spill_max_rows",
	    "Rows OF.TABLE spills at most; a larger result is #N/A and belongs in a table.", LogicalType::INTEGER,
	    Value::INTEGER(10000));
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

	TableFunction run("ofexcel_run", {LogicalType::VARCHAR}, ScanRows, RunBind, RunInit);
	AddNamedParameter(run, "params", LogicalType::ANY);
	AddNamedParameter(run, "connection", LogicalType::VARCHAR);
	loader.RegisterFunction(run);
	loader.RegisterFunction(RowsFunction("ofexcel_runs", RunsBind, MetaTableInit));
	loader.RegisterFunction(RowsFunction("ofexcel_datasets", DatasetsBind, MetaTableInit));
	loader.RegisterFunction(RowsFunction("ofexcel_missing", MissingBind, MetaTableInit));

	duckdb::ScalarFunction call("ofexcel_call", {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR},
	                            LogicalType::VARCHAR, CallFunction);
	// It reads the database: never folded into a constant.
	call.SetVolatile();
	call.SetNullHandling(duckdb::FunctionNullHandling::SPECIAL_HANDLING);
	loader.RegisterFunction(call);

	loader.RegisterFunction(RowsFunction("ofexcel_stop", StopBind, StopInit));
	loader.RegisterFunction(RowsFunction("ofexcel_status", StatusBind, StatusInit));
}

} // namespace ofexcel
