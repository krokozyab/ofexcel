// A Local run (CONTRACTS.md §5.3, §3.4, §7; SPEC.md §4.2, §7.5): its
// dependencies run or found first, short names for them as temporary views,
// the SQL run in DuckDB with $name bound, and the result stored as §3.2 stores
// its contract types, the same as an Oracle result.

#include "run_work.hpp"

#include "duckdb/main/appender.hpp"
#include "ofexcel/db_util.hpp"
#include "ofexcel/local_types.hpp"
#include "ofexcel/local_validator.hpp"
#include "ofexcel/param_values.hpp"
#include "ofexcel/query_registry.hpp"
#include "ofexcel/result_types.hpp"

#include <algorithm>
#include <iostream>

namespace ofexcel {

namespace {

//! Digits every BIGINT holds; an integer parameter with more is bound as HUGEINT.
constexpr size_t BIGINT_DIGITS = 18;

std::string FirstLine(const std::string &text) {
	return text.substr(0, text.find('\n'));
}

} // namespace

ApiError LocalSqlError(const duckdb::ErrorData &error) {
	const auto message = FirstLine(error.RawMessage());
	switch (error.Type()) {
	case duckdb::ExceptionType::PARSER:
	case duckdb::ExceptionType::BINDER:
	case duckdb::ExceptionType::CATALOG:
		return ApiError("E_SQL_COMPILE", 422, "The query does not compile: " + message);
	case duckdb::ExceptionType::CONVERSION:
	case duckdb::ExceptionType::OUT_OF_RANGE:
	case duckdb::ExceptionType::INVALID_INPUT:
		return ApiError("E_SQL_DATA", 422, "The query failed on its data: " + message);
	case duckdb::ExceptionType::OUT_OF_MEMORY:
		return ApiError("E_OUT_OF_MEMORY", 507, "DuckDB is out of memory: " + message);
	default:
		std::cerr << "ofexceld: Local query error: " << error.RawMessage() << std::endl;
		return ApiError("E_INTERNAL", 500, "ofexceld internal error: " + message);
	}
}

bool IsIntegerType(const duckdb::LogicalType &type) {
	switch (type.id()) {
	case duckdb::LogicalTypeId::TINYINT:
	case duckdb::LogicalTypeId::SMALLINT:
	case duckdb::LogicalTypeId::INTEGER:
	case duckdb::LogicalTypeId::BIGINT:
	case duckdb::LogicalTypeId::UTINYINT:
	case duckdb::LogicalTypeId::USMALLINT:
	case duckdb::LogicalTypeId::UINTEGER:
	case duckdb::LogicalTypeId::UBIGINT:
	case duckdb::LogicalTypeId::HUGEINT:
	case duckdb::LogicalTypeId::UHUGEINT:
		return true;
	default:
		return false;
	}
}

duckdb::LogicalType LocalStageType(const duckdb::LogicalType &type) {
	using duckdb::LogicalType;
	using duckdb::LogicalTypeId;
	if (IsIntegerType(type)) {
		// Kept exactly: the values decide between integer and id (§3.4).
		return LogicalType(type.id());
	}
	switch (type.id()) {
	case LogicalTypeId::BOOLEAN:
		return LogicalType::BOOLEAN;
	case LogicalTypeId::DECIMAL:
		return type;
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DOUBLE:
		return LogicalType::DOUBLE;
	case LogicalTypeId::DATE:
		return LogicalType::DATE;
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP_NS:
	case LogicalTypeId::TIMESTAMP_SEC:
		return LogicalType::TIMESTAMP;
	case LogicalTypeId::TIMESTAMP_TZ:
	case LogicalTypeId::TIMESTAMP_TZ_NS:
		return LogicalType::TIMESTAMP_TZ;
	default:
		return LogicalType::VARCHAR;
	}
}

namespace {

//! The DuckDB type a parameter is bound as (§5.3): the type closest to its
//! contract type. id and enum are strings, as an id column is stored (§3.2).
duckdb::LogicalType BoundType(const ParamType &type, const std::vector<std::string> &values) {
	switch (type.kind) {
	case ParamKind::INTEGER:
		for (const auto &value : values) {
			const auto digits = value.size() - (!value.empty() && value[0] == '-' ? 1 : 0);
			if (digits > BIGINT_DIGITS) {
				return duckdb::LogicalType::HUGEINT;
			}
		}
		return duckdb::LogicalType::BIGINT;
	case ParamKind::NUMBER:
		return duckdb::LogicalType::DOUBLE;
	case ParamKind::DECIMAL:
		return duckdb::LogicalType::DECIMAL(static_cast<uint8_t>(type.precision), static_cast<uint8_t>(type.scale));
	case ParamKind::DATE:
		return duckdb::LogicalType::DATE;
	case ParamKind::TIMESTAMP:
		return duckdb::LogicalType::TIMESTAMP;
	case ParamKind::BOOLEAN:
		return duckdb::LogicalType::BOOLEAN;
	default:
		return duckdb::LogicalType::VARCHAR;
	}
}

//! A canonical value (§3.1) as a DuckDB value; list<T> as a LIST.
duckdb::Value BoundValue(const ParamType &type, const CanonicalValue &value) {
	if (type.kind == ParamKind::LIST) {
		const auto element = BoundType(*type.element, value.list);
		duckdb::vector<duckdb::Value> elements;
		for (const auto &text : value.list) {
			elements.push_back(duckdb::Value(text).DefaultCastAs(element));
		}
		return duckdb::Value::LIST(element, std::move(elements));
	}
	return duckdb::Value(value.scalar).DefaultCastAs(BoundType(type, {value.scalar}));
}

//! A canonical value as the add-in would send it, for a dependency's
//! argument: a number as a JSON number, so that "1e+20" reads back; anything
//! else as a string, which every other type accepts in its canonical form.
ParamInput InputOf(const ParamType &type, const CanonicalValue &value) {
	const auto scalar = [](const ParamType &scalar_type, const std::string &text) {
		return scalar_type.kind == ParamKind::NUMBER ? ParamInput::Number(text) : ParamInput::String(text);
	};
	if (type.kind == ParamKind::LIST) {
		std::vector<ParamInput> elements;
		for (const auto &text : value.list) {
			elements.push_back(scalar(*type.element, text));
		}
		return ParamInput::Array(std::move(elements));
	}
	return scalar(type, value.scalar);
}

const QueryParam *FindParam(const std::vector<QueryParam> &params, const std::string &name) {
	for (const auto &param : params) {
		if (param.name == Lower(name)) {
			return &param;
		}
	}
	return nullptr;
}

//! The values `dependency` is run with: its arguments, $name taken from the
//! parent's own values.
std::map<std::string, ParamInput> DependencyInputs(const Dependency &dependency, const std::vector<QueryParam> &params,
                                                   const std::map<std::string, CanonicalValue> &values) {
	std::map<std::string, ParamInput> inputs;
	for (const auto &argument : dependency.arguments) {
		switch (argument.kind) {
		case DependencyArgument::Kind::PARAM: {
			// The parser checked that $name is declared.
			const auto *param = FindParam(params, argument.value);
			inputs[argument.name] = InputOf(param->type, values.at(param->name));
			break;
		}
		case DependencyArgument::Kind::NUMBER:
			inputs[argument.name] = ParamInput::Number(argument.value);
			break;
		case DependencyArgument::Kind::BOOLEAN:
			inputs[argument.name] = ParamInput::Boolean(argument.value == "true");
			break;
		case DependencyArgument::Kind::STRING:
		case DependencyArgument::Kind::DATE:
			inputs[argument.name] = ParamInput::String(argument.value);
			break;
		}
	}
	return inputs;
}

bool ImportExists(duckdb::Connection &connection, const std::string &name) {
	return !QueryRows(connection,
	                  "SELECT 1 FROM duckdb_tables() WHERE database_name = current_database() AND "
	                  "schema_name = 'xl_import' AND lower(table_name) = lower($1)",
	                  {duckdb::Value(name)})
	            .empty();
}

bool InstanceReady(duckdb::Connection &connection, const std::string &instance_key) {
	return !QueryRows(connection,
	                  "SELECT 1 FROM meta.dataset_instances WHERE instance_key = $1 AND status = 'ready' "
	                  "AND current_table IS NOT NULL",
	                  {duckdb::Value(instance_key)})
	            .empty();
}

JsonObject ParamsObject(const std::map<std::string, CanonicalValue> &values) {
	JsonObject params;
	for (const auto &entry : values) {
		if (entry.second.is_list) {
			auto elements = params.SetArray(entry.first);
			for (const auto &text : entry.second.list) {
				elements.AddString(text);
			}
		} else {
			params.Set(entry.first, entry.second.scalar);
		}
	}
	return params;
}

//! A dependency without data, for E_DEPENDENCY_MISSING and meta.missing.
struct Missing {
	Dependency::Kind kind = Dependency::Kind::QUERY;
	std::string name;
	//! A registered query: what refreshing it means.
	const DependencyPlan *dependency = nullptr;
	std::map<std::string, CanonicalValue> values;
};

//! §7.5: what a workbook misses, so that Smart Refresh can fetch it.
void RecordMissing(duckdb::Connection &connection, const std::string &workbook_id,
                   const std::vector<Missing> &missing) {
	if (workbook_id.empty()) {
		return;
	}
	std::lock_guard<std::mutex> guard(MetaWriteLock());
	InTransaction(connection, [&]() {
		const auto now = Now();
		for (const auto &entry : missing) {
			if (!entry.dependency) {
				continue;
			}
			const auto &request = entry.dependency->request;
			QueryRows(connection,
			          "INSERT INTO meta.missing (workbook_id, instance_key, query_id, definition_hash, "
			          "connection_name, params_json, first_seen, last_seen, requests) "
			          "VALUES ($1, $2, $3, $4, $5, $6, $7, $7, 1) "
			          "ON CONFLICT (workbook_id, instance_key) DO UPDATE SET last_seen = excluded.last_seen, "
			          "requests = requests + 1",
			          {duckdb::Value(workbook_id), duckdb::Value(entry.dependency->instance_key),
			           duckdb::Value(request.definition.query_id),
			           duckdb::Value(request.definition.definition_hash),
			           duckdb::Value(request.connection.empty() ? LOCAL_CONNECTION_NAME : request.connection),
			           duckdb::Value(CanonicalParams(entry.values)), now});
		}
	});
}

[[noreturn]] void RefuseMissing(duckdb::Connection &connection, const QueryHeader &header, const RunRequest &request,
                                const std::vector<Missing> &missing) {
	RecordMissing(connection, request.workbook_id, missing);
	JsonObject details;
	auto list = details.SetArray("missing");
	std::string names;
	for (const auto &entry : missing) {
		JsonObject item;
		const bool import = entry.kind == Dependency::Kind::IMPORT;
		item.Set("kind", import ? "import" : "query").Set("name", entry.name);
		if (entry.dependency) {
			const auto &dependency = *entry.dependency;
			JsonObject query;
			query.Set("id", dependency.request.definition.query_id)
			    .Set("hash", dependency.request.definition.definition_hash);
			item.Set("query", query)
			    .Set("connection",
			         dependency.request.connection.empty() ? LOCAL_CONNECTION_NAME : dependency.request.connection)
			    .Set("params", ParamsObject(entry.values))
			    .Set("instance_key", dependency.instance_key);
		}
		list.AddObject(item);
		names += (names.empty() ? "" : ", ") +
		         (import ? "xl_import." + entry.name + " (import the table from the sheet)"
		                 : entry.dependency ? entry.name : entry.name + " (no such query)");
	}
	throw ApiError("E_DEPENDENCY_MISSING", 409,
	               "Query " + header.id + " needs data that is not loaded: " + names + ". Refresh it first",
	               std::move(details));
}

} // namespace

duckdb::identifier_map_t<duckdb::BoundParameterData> BindLocalParams(const duckdb::PreparedStatement &prepared,
                                                                     const Plan &plan) {
	duckdb::identifier_map_t<duckdb::BoundParameterData> named;
	for (const auto &entry : prepared.GetNamedParameterMap()) {
		const auto &name = entry.first.GetIdentifierName();
		const auto *param = FindParam(plan.params, name);
		if (!param) {
			// The validator refuses this (V8); DuckDB found one it did not.
			throw ApiError("E_VALIDATION", 422, "V8: $" + name + " is not declared in @param");
		}
		auto value = BoundValue(param->type, plan.values.at(param->name));
		auto type = value.type();
		named[entry.first] = duckdb::BoundParameterData(std::move(value), std::move(type));
	}
	return named;
}

void CreateDependencyViews(duckdb::Connection &reader, duckdb::Connection &meta,
                           const std::vector<DependencyPlan> &depends) {
	for (const auto &dependency : depends) {
		std::string source;
		if (dependency.kind == Dependency::Kind::QUERY) {
			const auto rows = QueryRows(meta,
			                            "SELECT current_table FROM meta.dataset_instances WHERE instance_key = $1 "
			                            "AND status = 'ready' AND current_table IS NOT NULL",
			                            {duckdb::Value(dependency.instance_key)});
			if (rows.empty()) {
				throw ApiError("E_DEPENDENCY_MISSING", 409,
				               "Dependency " + dependency.name + " has no data: refresh it first");
			}
			source = "ds." + QuoteIdentifier(rows[0][0].ToString());
		} else {
			if (!ImportExists(meta, dependency.name)) {
				throw ApiError("E_DEPENDENCY_MISSING", 409,
				               "xl_import." + dependency.name + " is not loaded: import the table from the sheet");
			}
			source = "xl_import." + QuoteIdentifier(dependency.name);
		}
		Execute(reader, "CREATE TEMP VIEW " + QuoteIdentifier(dependency.name) + " AS SELECT * FROM " + source);
	}
}

duckdb::Value DeclaredLocalValue(const std::string &name, const QueryColumn &column, const duckdb::Value &value,
                                 int64_t row) {
	if (value.IsNull()) {
		return duckdb::Value(duckdb::LogicalType::VARCHAR);
	}
	// A TIMESTAMPTZ is UTC (no time zone extension is loaded): written with
	// its offset, it reads as TSTZ.
	const auto text = value.type().id() == duckdb::LogicalTypeId::TIMESTAMP_TZ
	                      ? value.DefaultCastAs(duckdb::LogicalType::TIMESTAMP).ToString() + " +00:00"
	                      : value.ToString();
	if (column.type.kind == ColumnKind::STRING || column.type.kind == ColumnKind::TEXT) {
		return duckdb::Value(text);
	}
	std::string stored;
	if (!text.empty() && ConvertOracleValue(column.type, text, stored) == StoredValue::VALUE) {
		return duckdb::Value(stored);
	}
	throw ApiError("E_TYPE_DECLARED_MISMATCH", 422,
	               "column " + name + " is declared as " + column.type.ToString() + ", but in row " +
	                   std::to_string(row) + " the value is \"" + text + "\"");
}

namespace {

class LocalRun : public RunWork {
public:
	using RunWork::RunWork;

protected:
	void Fetch(Outcome &outcome) override {
		RunDependencies();
		ThrowIfStopping();
		CreateDependencyViews(reader, writer, plan.depends);
		const auto error = ReadIntoStage(outcome);
		if (error.HasError()) {
			if (Stopping()) {
				throw Stopped();
			}
			throw LocalSqlError(error);
		}
		ThrowIfStopping();
		StoreRows(outcome);
	}

	ApiError MapError(const duckdb::ErrorData &error) const override {
		return LocalSqlError(error);
	}

private:
	struct Started {
		const DependencyPlan *dependency;
		std::string run_id;
		//! Another run had started it; cancelling this run leaves it alone.
		bool joined;
	};

	void CancelStarted(const std::vector<Started> &started) {
		for (const auto &one : started) {
			if (!one.joined) {
				RunEngine::Get().Cancel(one.run_id);
			}
		}
	}

	//! §8.3 `deps`: the dependencies Start chose run first, each a run of its
	//! own with this one as its parent. Any of them failing fails this run.
	void RunDependencies() {
		auto &engine = RunEngine::Get();
		std::vector<Started> started;
		for (const auto &dependency : plan.depends) {
			if (!dependency.start) {
				continue;
			}
			auto request = dependency.request;
			request.parent_run_id = plan.run_id;
			try {
				const auto start = engine.Start(db, request);
				started.push_back({&dependency, start.run_id, start.joined});
			} catch (const ApiError &error) {
				CancelStarted(started);
				throw ApiError("E_DEPENDENCY_FAILED", 500,
				               "Dependency " + dependency.name + " could not start: " + error.code + ": " +
				                   error.what());
			}
		}
		for (const auto &one : started) {
			while (!engine.WaitFor(one.run_id, INTERRUPT_REPEAT)) {
				if (Stopping()) {
					CancelStarted(started);
					throw Stopped();
				}
			}
		}
		for (const auto &one : started) {
			const auto rows =
			    QueryRows(writer, "SELECT status, error_code, error_message FROM meta.runs WHERE run_id = $1",
			              {duckdb::Value(one.run_id)});
			if (!rows.empty() && rows[0][0].ToString() == "succeeded") {
				continue;
			}
			std::string why = "it is not recorded";
			if (!rows.empty()) {
				why = rows[0][0].ToString();
				if (!rows[0][1].IsNull()) {
					why += ", " + rows[0][1].ToString() + ": " + (rows[0][2].IsNull() ? "" : rows[0][2].ToString());
				}
			}
			throw ApiError("E_DEPENDENCY_FAILED", 500,
			               "Dependency " + one.dependency->name + " failed (run " + one.run_id + "): " + why);
		}
	}

	//! The body as a prepared statement with $name bound (§5.3).
	duckdb::ErrorData ReadIntoStage(Outcome &outcome) {
		auto prepared = reader.Prepare(plan.body);
		if (prepared->HasError()) {
			return prepared->GetErrorObject();
		}
		auto named = BindLocalParams(*prepared, plan);
		return StreamIntoStage([&]() { return prepared->Submit(named); }, LocalStageType, outcome);
	}

	const QueryColumn *Declared(const std::string &name) const {
		const auto lower = Lower(name);
		for (const auto &column : plan.declared) {
			if (Lower(column.name) == lower) {
				return &column;
			}
		}
		return nullptr;
	}

	//! Integer columns holding a value with |value| >= 10^15 (§3.4).
	std::vector<bool> LargeIntegers() {
		std::vector<bool> large(types.size(), false);
		std::string select;
		std::vector<size_t> integer_columns;
		for (size_t i = 0; i < types.size(); i++) {
			if (!IsIntegerType(types[i])) {
				continue;
			}
			const auto column = "c" + std::to_string(i);
			select += std::string(select.empty() ? "" : ", ") + "coalesce(bool_or(" + column +
			          " >= " + std::to_string(LOCAL_ID_FROM) + " OR " + column + " <= -" +
			          std::to_string(LOCAL_ID_FROM) + "), false)";
			integer_columns.push_back(i);
		}
		if (integer_columns.empty()) {
			return large;
		}
		const auto rows = QueryRows(scanner, "SELECT " + select + " FROM ds." + QuoteIdentifier(stage));
		for (size_t k = 0; k < integer_columns.size(); k++) {
			large[integer_columns[k]] = rows[0][k].GetValue<bool>();
		}
		return large;
	}

	void StoreRows(Outcome &outcome) {
		for (size_t i = 0; i < names.size(); i++) {
			for (size_t j = 0; j < i; j++) {
				if (Lower(names[i]) == Lower(names[j])) {
					throw ApiError("E_SQL_COMPILE", 422,
					               "The result has two columns named " + names[i] + ": give them different names");
				}
			}
		}
		const auto large = LargeIntegers();
		std::vector<SchemaColumn> schema;
		std::vector<const QueryColumn *> declared;
		for (size_t i = 0; i < names.size(); i++) {
			declared.push_back(Declared(names[i]));
			if (declared.back()) {
				schema.push_back({names[i], declared.back()->type, declared.back()->header});
				continue;
			}
			LocalColumnStats stats;
			stats.has_value_from_1e15 = large[i];
			const auto mapping = MapLocalType(types[i], names[i], stats);
			if (mapping.cast_to_varchar) {
				warnings.push_back({WARNING_TYPE_WIDENED, mapping.warning});
			}
			schema.push_back({names[i], mapping.type, ""});
		}

		CreateTarget(schema, outcome);
		duckdb::Appender appender(writer, duckdb::Identifier(DATASET_SCHEMA), duckdb::Identifier(outcome.result_table));
		int64_t rows_before = 0;
		ScanStage([&](duckdb::DataChunk &chunk) {
			// Declared columns become the text §3.3 stores; the others go as they
			// are, and the appender casts them to their storage type (§3.2).
			duckdb::vector<duckdb::LogicalType> chunk_types;
			for (duckdb::idx_t i = 0; i < chunk.ColumnCount(); i++) {
				chunk_types.push_back(declared[i] ? duckdb::LogicalType::VARCHAR : chunk.data[i].GetType());
			}
			duckdb::DataChunk converted;
			converted.Initialize(duckdb::Allocator::DefaultAllocator(), chunk_types);
			for (duckdb::idx_t i = 0; i < chunk.ColumnCount(); i++) {
				if (!declared[i]) {
					converted.data[i].Reference(chunk.data[i]);
					continue;
				}
				for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
					converted.SetValue(i, row,
					                   DeclaredLocalValue(names[i], *declared[i], chunk.GetValue(i, row),
					                                      rows_before + static_cast<int64_t>(row) + 1));
				}
			}
			converted.SetCardinality(chunk.size());
			appender.AppendDataChunk(converted);
			rows_before += static_cast<int64_t>(chunk.size());
		});
		appender.Close();
		ThrowIfStopping();
		Commit(outcome, schema, true);
	}
};

} // namespace

std::unique_ptr<RunWork> LocalWork(duckdb::DatabaseInstance &db, RunEngine::Active &run, const Plan &plan) {
	return std::unique_ptr<RunWork>(new LocalRun(db, run, plan));
}

void PlanLocalRun(duckdb::Connection &connection, const QueryHeader &header, const RunRequest &request, Plan &plan) {
	const auto verdict = ValidateLocalSql(header.body, LocalSqlScope::FromHeader(header));
	if (!verdict.ok) {
		throw ApiError(verdict.code, 422, verdict.message);
	}
	auto normalized = NormalizeParams(header.params, request.params);
	if (!normalized.ok) {
		throw ApiError(normalized.code, 422, normalized.message);
	}
	plan.engine = Engine::LOCAL;
	plan.connection = LOCAL_CONNECTION_NAME;
	plan.body = header.body;
	plan.params = header.params;
	plan.values = normalized.values;
	const auto &used = verdict.params_in_text;
	const auto passed_on = ParamsPassedOn(header);
	for (const auto &param : header.params) {
		if (std::find(used.begin(), used.end(), param.name) == used.end() &&
		    std::find(passed_on.begin(), passed_on.end(), param.name) == passed_on.end()) {
			plan.warnings.push_back(
			    {WARNING_PARAM_UNUSED, "parameter " + param.name + " is declared but not used in the query text"});
		}
	}

	auto chain = request.ancestry;
	chain.push_back(header.id);
	std::vector<Missing> missing;
	// Keeps the pointers `missing` takes into it in place.
	plan.depends.reserve(header.depends.size());
	for (const auto &depends : header.depends) {
		DependencyPlan dependency;
		dependency.kind = depends.kind;
		dependency.name = depends.name;
		if (depends.kind == Dependency::Kind::IMPORT) {
			if (!ImportExists(connection, depends.name)) {
				missing.push_back({depends.kind, depends.name, nullptr, {}});
			}
			plan.depends.push_back(std::move(dependency));
			continue;
		}

		auto &definition = dependency.request.definition;
		QueryScope scope;
		scope.workbook_id = request.workbook_id;
		if (!FindQuery(connection, depends.name, scope, definition)) {
			missing.push_back({depends.kind, depends.name, nullptr, {}});
			continue;
		}
		for (const auto &outer : chain) {
			if (Lower(outer) == Lower(definition.query_id)) {
				std::string path;
				for (const auto &step : chain) {
					path += step + " → ";
				}
				throw ApiError("E_DEPENDENCY_FAILED", 422, "Dependency cycle: " + path + definition.query_id);
			}
		}
		const auto parsed = ParseQueryFile(definition.text);
		if (!parsed.Ok()) {
			throw ApiError(parsed.errors[0].code, 422,
			               "Dependency " + definition.query_id + " cannot be parsed: " + parsed.errors[0].ToString());
		}
		const auto &dependency_header = parsed.header;
		auto &inputs = dependency.request.params;
		inputs = DependencyInputs(depends, header.params, plan.values);
		const auto values = NormalizeParams(dependency_header.params, inputs);
		if (!values.ok) {
			throw ApiError(values.code, 422, "Dependency " + definition.query_id + ": " + values.message);
		}
		std::string connection_name = LOCAL_CONNECTION_NAME;
		if (dependency_header.engine == Engine::ORACLE) {
			connection_name = request.connection.empty() ? dependency_header.connection : request.connection;
			if (connection_name.empty()) {
				throw ApiError("E_BAD_REQUEST", 422,
				               "No connection for dependency " + definition.query_id +
				                   ": pass connection or declare @connection in " + definition.query_id);
			}
			dependency.request.connection = connection_name;
		}
		dependency.request.workbook_id = request.workbook_id;
		dependency.request.deps = request.deps;
		dependency.request.ancestry = chain;
		dependency.instance_key = InstanceKey(definition.query_id, definition.definition_hash, connection_name,
		                                      CanonicalParams(values.values));
		const bool ready = InstanceReady(connection, dependency.instance_key);
		dependency.start = request.deps == DepsMode::ALL || (request.deps == DepsMode::MISSING && !ready);
		const bool absent = request.deps == DepsMode::NONE && !ready;
		plan.depends.push_back(std::move(dependency));
		if (absent) {
			missing.push_back({depends.kind, depends.name, &plan.depends.back(), values.values});
		}
	}
	if (!missing.empty()) {
		RefuseMissing(connection, header, request, missing);
	}
}

} // namespace ofexcel
