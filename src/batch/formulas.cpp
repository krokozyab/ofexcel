// Formulas a batch at a time (CONTRACTS.md §10.2, §10.3; SPEC.md §7.5, §7.7,
// §7.8). Each call: resolve the query → trust → arguments → instance_key →
// the stored instance, or a Local query evaluated over its dependencies.
// Local evaluations of one query form a group: prepared once, run once per
// set of arguments, so that a data error stays with its own cells; any other
// error ends the group at once (DECISIONS.md R10).

#include "ofexcel/formulas.hpp"

#include "../run/run_work.hpp"
#include "ofexcel/api_error.hpp"
#include "ofexcel/datasets.hpp"
#include "ofexcel/db_util.hpp"
#include "ofexcel/hashing.hpp"
#include "ofexcel/query_file.hpp"
#include "ofexcel/query_registry.hpp"
#include "ofexcel/utf8.hpp"

#include <cmath>
#include <ctime>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace ofexcel {

namespace {

const char *const INVALID_VALUE = "invalidValue";
const char *const INVALID_NAME = "invalidName";
const char *const NOT_AVAILABLE = "notAvailable";

//! ofexcel_spill_max_rows' default (§10.2).
constexpr int64_t DEFAULT_SPILL_MAX_ROWS = 10000;
//! What a cell holds at most, in characters (§11.4).
constexpr size_t MAX_CELL_CHARACTERS = 32767;
//! Serial numbers of 1970-01-01 in the 1900 and the 1904 date system.
constexpr double EPOCH_SERIAL_1900 = 25569;
constexpr double EPOCH_SERIAL_1904 = 24107;
constexpr double MICROS_PER_DAY = 86400.0 * 1000000.0;

//! How long the server keeps results (SPEC.md §7.7).
constexpr size_t CACHE_ENTRIES = 512;
constexpr size_t CACHE_CELLS = 2000000;

const char CORE_FUNCTIONS[] = R"QEXCEL({
  "functions": [
    { "id": "TABLE", "name": "TABLE",
      "description": "QExcel query result as an array; the first row holds the headers",
      "parameters": [
        { "name": "query", "type": "string", "dimensionality": "scalar" },
        { "name": "args", "type": "any", "dimensionality": "matrix", "repeating": true, "optional": true } ],
      "result": { "type": "any", "dimensionality": "matrix" } },
    { "id": "CALL", "name": "CALL",
      "description": "Scalar value of a QExcel query (a 1 × 1 result)",
      "parameters": [
        { "name": "query", "type": "string", "dimensionality": "scalar" },
        { "name": "args", "type": "any", "dimensionality": "matrix", "repeating": true, "optional": true } ],
      "result": { "type": "any", "dimensionality": "scalar" } },
    { "id": "ASOF", "name": "ASOF",
      "description": "Time the query data was last fetched successfully",
      "parameters": [
        { "name": "query", "type": "string", "dimensionality": "scalar", "optional": true },
        { "name": "args", "type": "any", "dimensionality": "matrix", "repeating": true, "optional": true } ],
      "result": { "type": "number", "dimensionality": "scalar" } },
    { "id": "STATUS", "name": "STATUS",
      "description": "QExcel status: OK, NOT_SIGNED_IN, STALE, OFFLINE",
      "parameters": [],
      "result": { "type": "string", "dimensionality": "scalar" } }
  ]
}
)QEXCEL";

// --- The server's result cache (§10.3) ------------------------------------------

class ResultCache {
public:
	static ResultCache &Get() {
		static ResultCache cache;
		return cache;
	}

	bool Find(const std::string &key, FormulaResult &found) {
		std::lock_guard<std::mutex> lock(mutex);
		const auto entry = index.find(key);
		if (entry == index.end()) {
			return false;
		}
		entries.splice(entries.begin(), entries, entry->second);
		found = entry->second->second;
		return true;
	}

	void Put(const std::string &key, const FormulaResult &result) {
		const auto size = Cells(result);
		if (size > CACHE_CELLS) {
			return;
		}
		std::lock_guard<std::mutex> lock(mutex);
		const auto existing = index.find(key);
		if (existing != index.end()) {
			cells -= Cells(existing->second->second);
			entries.erase(existing->second);
			index.erase(existing);
		}
		entries.emplace_front(key, result);
		index[key] = entries.begin();
		cells += size;
		while (entries.size() > CACHE_ENTRIES || cells > CACHE_CELLS) {
			cells -= Cells(entries.back().second);
			index.erase(entries.back().first);
			entries.pop_back();
		}
	}

	void Clear() {
		std::lock_guard<std::mutex> lock(mutex);
		entries.clear();
		index.clear();
		cells = 0;
	}

private:
	static size_t Cells(const FormulaResult &result) {
		size_t count = 1;
		for (const auto &row : result.rows) {
			count += row.size();
		}
		return count;
	}

	std::mutex mutex;
	std::list<std::pair<std::string, FormulaResult>> entries;
	std::unordered_map<std::string, std::list<std::pair<std::string, FormulaResult>>::iterator> index;
	size_t cells = 0;
};

// --- Values for Excel (§11.4) ------------------------------------------------------

ExcelValue Text(std::string text) {
	if (text.size() > MAX_CELL_CHARACTERS && IsValidUtf8(text) && Utf8Length(text) > MAX_CELL_CHARACTERS) {
		// Cut after MAX_CELL_CHARACTERS code points, never inside one.
		size_t characters = 0;
		size_t at = 0;
		while (at < text.size() && characters < MAX_CELL_CHARACTERS) {
			at++;
			while (at < text.size() && (static_cast<unsigned char>(text[at]) & 0xC0) == 0x80) {
				at++;
			}
			characters++;
		}
		text.resize(at);
	}
	ExcelValue value;
	value.text = std::move(text);
	return value;
}

ExcelValue Number(double number) {
	if (!std::isfinite(number)) {
		return Text("");
	}
	ExcelValue value;
	value.kind = ExcelValue::Kind::NUMBER;
	value.number = number;
	return value;
}

double EpochSerial(bool date1904) {
	return date1904 ? EPOCH_SERIAL_1904 : EPOCH_SERIAL_1900;
}

//! A stored value (§3.2) as Excel shows it; NULL as an empty string.
ExcelValue ToExcel(const ColumnType &type, const duckdb::Value &value, bool date1904) {
	if (value.IsNull()) {
		return Text("");
	}
	switch (type.kind) {
	case ColumnKind::INTEGER:
	case ColumnKind::NUMBER:
	case ColumnKind::DECIMAL:
		return Number(value.GetValue<double>());
	case ColumnKind::DATE:
		return Number(EpochSerial(date1904) + value.GetValue<duckdb::date_t>().days);
	case ColumnKind::TIMESTAMP:
	case ColumnKind::TIMESTAMPTZ:
		return Number(EpochSerial(date1904) +
		              static_cast<double>(value.GetValue<duckdb::timestamp_t>().value) / MICROS_PER_DAY);
	case ColumnKind::BOOLEAN: {
		ExcelValue result;
		result.kind = ExcelValue::Kind::BOOLEAN;
		result.boolean = value.GetValue<bool>();
		return result;
	}
	default:
		return Text(value.ToString());
	}
}

//! A TIMESTAMP in UTC as a serial number of this machine's local time (§10.2
//! OF.ASOF).
ExcelValue LocalSerial(const duckdb::Value &utc, bool date1904) {
	const auto micros = utc.GetValue<duckdb::timestamp_t>().value;
	const auto seconds = static_cast<std::time_t>(micros / 1000000);
	std::tm local {};
#ifdef _WIN32
	localtime_s(&local, &seconds);
#else
	localtime_r(&seconds, &local);
#endif
	const auto days = duckdb::Date::FromDate(local.tm_year + 1900, local.tm_mon + 1, local.tm_mday).days;
	const auto day_seconds = local.tm_hour * 3600 + local.tm_min * 60 + local.tm_sec;
	return Number(EpochSerial(date1904) + days + (day_seconds + static_cast<double>(micros % 1000000) / 1e6) / 86400.0);
}

// --- One call ------------------------------------------------------------------

FormulaResult Failed(int64_t i, const char *error, std::string message) {
	FormulaResult result;
	result.i = i;
	result.error = error;
	result.message = std::move(message);
	return result;
}

//! §14's Excel column for an error code.
const char *ExcelError(const std::string &code) {
	if (code == "E_NOT_FOUND") {
		return INVALID_NAME;
	}
	if (code.rfind("E_PARAM_", 0) == 0 || code == "E_QUERY_PARSE" || code == "E_UNSUPPORTED" ||
	    code == "E_VALIDATION" || code == "E_SQL_DATA" || code == "E_BAD_REQUEST") {
		return INVALID_VALUE;
	}
	return NOT_AVAILABLE;
}

bool IsEmptyCell(const ParamInput &cell) {
	return cell.kind == ParamInput::Kind::MISSING || cell.kind == ParamInput::Kind::NULL_VALUE ||
	       (cell.kind == ParamInput::Kind::STRING && cell.text.empty());
}

//! §10.2: positional, in @param order; a scalar takes one cell, a list<T> the
//! matrix row by row without its empty cells. An empty cell or a missing
//! argument takes the default.
bool Arguments(const QueryHeader &header, const std::vector<CellMatrix> &args,
               std::map<std::string, ParamInput> &inputs, std::string &error) {
	if (args.size() > header.params.size()) {
		error = header.id + " takes " + std::to_string(header.params.size()) + " arguments, not " +
		        std::to_string(args.size());
		return false;
	}
	for (size_t k = 0; k < args.size(); k++) {
		const auto &param = header.params[k];
		const auto &matrix = args[k];
		if (param.type.kind == ParamKind::LIST) {
			std::vector<ParamInput> elements;
			for (const auto &row : matrix) {
				for (const auto &cell : row) {
					if (!IsEmptyCell(cell)) {
						elements.push_back(cell);
					}
				}
			}
			if (!elements.empty()) {
				inputs[param.name] = ParamInput::Array(std::move(elements));
			}
			continue;
		}
		if (matrix.empty()) {
			continue;
		}
		if (matrix.size() != 1 || matrix[0].size() != 1) {
			error = "argument " + std::to_string(k + 1) + " (" + param.name + ") must be a single cell";
			return false;
		}
		if (!IsEmptyCell(matrix[0][0])) {
			inputs[param.name] = matrix[0][0];
		}
	}
	return true;
}

//! A result's columns and rows, as stored or as evaluated.
struct Rows {
	std::vector<SchemaColumn> columns;
	std::vector<std::vector<duckdb::Value>> rows;
	//! There are more rows than ofexcel_spill_max_rows.
	bool too_large = false;
};

//! OF.TABLE or OF.CALL over `rows` (§10.2).
FormulaResult Shape(const FormulaCall &call, const Rows &rows, bool date1904) {
	FormulaResult result;
	result.i = call.i;
	if (call.kind == "call") {
		const auto row_count = rows.too_large ? std::string("more than ") + std::to_string(rows.rows.size())
		                                      : std::to_string(rows.rows.size());
		if (rows.too_large || rows.rows.size() != 1 || rows.columns.size() != 1) {
			return Failed(call.i, INVALID_VALUE,
			              "Expected a 1 × 1 result, got " + row_count + " × " + std::to_string(rows.columns.size()));
		}
		if (rows.rows[0][0].IsNull()) {
			return Failed(call.i, NOT_AVAILABLE, "The value is empty");
		}
		result.ok = true;
		result.rows = {{ToExcel(rows.columns[0].type, rows.rows[0][0], date1904)}};
		return result;
	}
	if (rows.too_large) {
		return Failed(call.i, NOT_AVAILABLE, "Result too large: load it into a table");
	}
	result.ok = true;
	result.matrix = true;
	std::vector<ExcelValue> headers;
	for (const auto &column : rows.columns) {
		headers.push_back(Text(column.header.empty() ? column.name : column.header));
	}
	result.rows.push_back(std::move(headers));
	for (const auto &stored : rows.rows) {
		std::vector<ExcelValue> row;
		for (size_t c = 0; c < rows.columns.size(); c++) {
			row.push_back(ToExcel(rows.columns[c].type, stored[c], date1904));
		}
		result.rows.push_back(std::move(row));
	}
	return result;
}

//! A call resolved as far as its instance_key.
struct Resolved {
	const FormulaCall *call = nullptr;
	QueryDefinition definition;
	std::shared_ptr<const QueryHeader> header;
	std::string connection;
	std::map<std::string, ParamInput> inputs;
	std::string params_json;
	std::string instance_key;
};

class Evaluator {
public:
	Evaluator(duckdb::DatabaseInstance &db_p, const BatchContext &context_p)
	    : db(db_p), connection(db_p), context(context_p) {
		const auto setting = TryQueryRows(connection, "SELECT current_setting('ofexcel_spill_max_rows')", {});
		if (setting.Ok() && !setting.rows.empty() && !setting.rows[0][0].IsNull()) {
			spill_max_rows = std::max<int64_t>(1, setting.rows[0][0].GetValue<int64_t>());
		}
	}

	std::vector<FormulaResult> Run(const std::vector<FormulaCall> &calls) {
		std::vector<FormulaResult> results(calls.size());
		for (size_t n = 0; n < calls.size(); n++) {
			try {
				Evaluate(calls[n], n, results[n]);
			} catch (const ApiError &error) {
				results[n] = Failed(calls[n].i, ExcelError(error.code), error.what());
			}
		}
		RunLocalGroups(results);
		return results;
	}

private:
	//! A Local query to evaluate for this call, after every call is resolved.
	struct LocalCall {
		size_t index;
		Resolved resolved;
		Plan plan;
		std::string cache_key;
	};

	void Evaluate(const FormulaCall &call, size_t index, FormulaResult &result) {
		Resolved resolved;
		resolved.call = &call;
		if (call.kind != "table" && call.kind != "call" && call.kind != "asof") {
			result = Failed(call.i, INVALID_VALUE, "Unknown formula kind " + call.kind);
			return;
		}
		QueryScope scope;
		scope.workbook_id = context.workbook_id;
		if (!FindQuery(connection, call.query, scope, resolved.definition)) {
			result = Failed(call.i, INVALID_NAME, "Unknown query " + call.query);
			return;
		}
		if (!IsDefinitionTrusted(connection, resolved.definition)) {
			result = Failed(call.i, NOT_AVAILABLE, "Query not trusted");
			return;
		}
		resolved.header = Header(resolved.definition);
		const auto &header = *resolved.header;
		std::string error;
		if (!Arguments(header, call.args, resolved.inputs, error)) {
			result = Failed(call.i, INVALID_VALUE, error);
			return;
		}
		const auto values = NormalizeParams(header.params, resolved.inputs);
		if (!values.ok) {
			result = Failed(call.i, INVALID_VALUE, values.message);
			return;
		}
		if (header.engine == Engine::LOCAL) {
			resolved.connection = LOCAL_CONNECTION_NAME;
		} else {
			resolved.connection = context.connection.empty() ? header.connection : context.connection;
			if (resolved.connection.empty()) {
				result = Failed(call.i, NOT_AVAILABLE, "No connection selected");
				return;
			}
		}
		resolved.params_json = CanonicalParams(values.values);
		resolved.instance_key = InstanceKey(header.id, resolved.definition.definition_hash, resolved.connection,
		                                    resolved.params_json);

		const auto instance =
		    QueryRows(connection,
		              "SELECT current_table, data_version, fetched_at FROM meta.dataset_instances "
		              "WHERE instance_key = $1 AND status = 'ready' AND current_table IS NOT NULL",
		              {duckdb::Value(resolved.instance_key)});
		if (!instance.empty()) {
			// What a run stored, whichever the engine.
			if (call.kind == "asof") {
				result = Asof(call, instance[0][2]);
				return;
			}
			const auto key = CacheKey(resolved, "i" + instance[0][1].ToString());
			if (ResultCache::Get().Find(key, result)) {
				result.i = call.i;
				return;
			}
			result = Shape(call, Stored(instance[0][0].ToString(), resolved.definition.definition_hash),
			               context.date1904);
			if (result.ok) {
				ResultCache::Get().Put(key, result);
			}
			return;
		}
		if (header.engine == Engine::ORACLE) {
			// A formula never fetches (§10.2): it says what is missing.
			if (call.kind != "asof") {
				RecordMissing(connection, context.workbook_id,
				              {{resolved.instance_key, header.id, resolved.definition.definition_hash,
				                resolved.connection, resolved.params_json}});
			}
			result = Failed(call.i, NOT_AVAILABLE, "No data for " + header.id + ": refresh it");
			return;
		}

		// A Local query without a stored instance is evaluated over its
		// dependencies' current data; one without them records what is missing.
		RunRequest request;
		request.params = resolved.inputs;
		request.connection = context.connection;
		request.workbook_id = context.workbook_id;
		request.deps = DepsMode::NONE;
		LocalCall local;
		local.index = index;
		local.plan.query_id = header.id;
		local.plan.definition_hash = resolved.definition.definition_hash;
		local.plan.declared = header.columns;
		PlanLocalRun(connection, header, request, local.plan);
		if (call.kind == "asof") {
			result = DependenciesAsof(call, local.plan);
			return;
		}
		local.cache_key = CacheKey(resolved, DependencyVersions(local.plan));
		if (ResultCache::Get().Find(local.cache_key, result)) {
			result.i = call.i;
			return;
		}
		local.resolved = std::move(resolved);
		local_calls.push_back(std::move(local));
	}

	std::shared_ptr<const QueryHeader> Header(const QueryDefinition &definition) {
		auto &header = headers[definition.definition_hash];
		if (!header) {
			const auto parsed = ParseQueryFile(definition.text);
			if (!parsed.Ok()) {
				throw ApiError(parsed.errors[0].code, 422,
				               "Query " + definition.query_id + " cannot be parsed: " + parsed.errors[0].ToString());
			}
			header = std::make_shared<const QueryHeader>(parsed.header);
		}
		return header;
	}

	//! §10.3: what the result depends on, and who may see it.
	std::string CacheKey(const Resolved &resolved, const std::string &versions) {
		std::string user;
		if (resolved.connection != LOCAL_CONNECTION_NAME) {
			auto found = users.find(resolved.connection);
			if (found == users.end()) {
				const auto value = FusionUser(connection, resolved.connection);
				found = users.emplace(resolved.connection, value.IsNull() ? "" : value.ToString()).first;
			}
			user = found->second;
		}
		const std::string parts[] = {resolved.call->kind,
		                             context.date1904 ? "1904" : "1900",
		                             resolved.definition.query_id,
		                             resolved.definition.definition_hash,
		                             resolved.connection,
		                             resolved.params_json,
		                             versions,
		                             user};
		std::string key;
		for (const auto &part : parts) {
			key += std::to_string(part.size()) + ":" + part;
		}
		return key;
	}

	//! data_version of every dependency, as they are now.
	std::string DependencyVersions(const Plan &plan) {
		std::string versions;
		for (const auto &dependency : plan.depends) {
			const auto rows =
			    dependency.kind == Dependency::Kind::QUERY
			        ? QueryRows(connection, "SELECT data_version FROM meta.dataset_instances WHERE instance_key = $1",
			                    {duckdb::Value(dependency.instance_key)})
			        : QueryRows(connection, "SELECT data_version FROM meta.imports WHERE name = $1",
			                    {duckdb::Value(dependency.name)});
			versions += (rows.empty() ? std::string("-") : rows[0][0].ToString()) + ",";
		}
		return versions;
	}

	FormulaResult Asof(const FormulaCall &call, const duckdb::Value &fetched_at) {
		if (fetched_at.IsNull()) {
			return Failed(call.i, NOT_AVAILABLE, "No data for " + call.query);
		}
		FormulaResult result;
		result.i = call.i;
		result.ok = true;
		result.rows = {{LocalSerial(fetched_at, context.date1904)}};
		return result;
	}

	//! A Local query without a stored instance is as old as its oldest
	//! dependency.
	FormulaResult DependenciesAsof(const FormulaCall &call, const Plan &plan) {
		duckdb::Value oldest(duckdb::LogicalType::TIMESTAMP);
		for (const auto &dependency : plan.depends) {
			const auto rows =
			    dependency.kind == Dependency::Kind::QUERY
			        ? QueryRows(connection, "SELECT fetched_at FROM meta.dataset_instances WHERE instance_key = $1",
			                    {duckdb::Value(dependency.instance_key)})
			        : QueryRows(connection, "SELECT imported_at FROM meta.imports WHERE name = $1",
			                    {duckdb::Value(dependency.name)});
			if (rows.empty() || rows[0][0].IsNull()) {
				continue;
			}
			if (oldest.IsNull() ||
			    rows[0][0].GetValue<duckdb::timestamp_t>() < oldest.GetValue<duckdb::timestamp_t>()) {
				oldest = rows[0][0];
			}
		}
		return Asof(call, oldest);
	}

	//! ds.<table>, as /rows reads it, as far as ofexcel_spill_max_rows + 1.
	Rows Stored(const std::string &table, const std::string &definition_hash) {
		Rows rows;
		rows.columns = StoredSchema(connection, table, definition_hash);
		if (rows.columns.empty()) {
			throw ApiError("E_GONE", 410, "The data was replaced while it was read; recalculate");
		}
		rows.rows = QueryRows(connection,
		                      "SELECT * FROM ds." + QuoteIdentifier(table) + " ORDER BY rowid LIMIT $1",
		                      {duckdb::Value::BIGINT(spill_max_rows + 1)});
		if (static_cast<int64_t>(rows.rows.size()) > spill_max_rows) {
			rows.rows.resize(static_cast<size_t>(spill_max_rows));
			rows.too_large = true;
		}
		return rows;
	}

	//! §7.8 as DECISIONS.md R10 has it: a group per query, prepared once and
	//! run per set of arguments. E_SQL_DATA stays with its own cells; any
	//! other error is the group's, and what is left of it is not run.
	void RunLocalGroups(std::vector<FormulaResult> &results) {
		std::map<std::string, std::vector<LocalCall *>> groups;
		std::vector<std::string> order;
		for (auto &local : local_calls) {
			const auto &hash = local.resolved.definition.definition_hash;
			if (groups.find(hash) == groups.end()) {
				order.push_back(hash);
			}
			groups[hash].push_back(&local);
		}
		if (order.empty()) {
			return;
		}
		WatchedReader watched(RunTimeoutSetting(connection));
		for (const auto &hash : order) {
			auto &group = groups[hash];
			LocalEvaluation evaluation(db, connection, *group[0]->resolved.header);
			// One run per instance, shared by its calls.
			std::map<std::string, LocalRows> done;
			std::unique_ptr<ApiError> group_error;
			for (auto *local : group) {
				const auto &key = local->resolved.instance_key;
				auto &result = results[local->index];
				if (group_error) {
					result = Failed(local->resolved.call->i, ExcelError(group_error->code), group_error->what());
					continue;
				}
				auto found = done.find(key);
				if (found == done.end()) {
					try {
						found = done.emplace(key, evaluation.Run(local->plan, spill_max_rows, watched)).first;
					} catch (const ApiError &error) {
						result = Failed(local->resolved.call->i, ExcelError(error.code), error.what());
						if (error.code != "E_SQL_DATA") {
							group_error = std::make_unique<ApiError>(error);
						}
						continue;
					}
				}
				Rows rows;
				rows.columns = found->second.columns;
				rows.rows = found->second.rows;
				rows.too_large = found->second.truncated;
				result = Shape(*local->resolved.call, rows, context.date1904);
				if (result.ok) {
					ResultCache::Get().Put(local->cache_key, result);
				}
			}
		}
	}

	duckdb::DatabaseInstance &db;
	duckdb::Connection connection;
	const BatchContext &context;
	int64_t spill_max_rows = DEFAULT_SPILL_MAX_ROWS;
	std::map<std::string, std::shared_ptr<const QueryHeader>> headers;
	//! connection → the Fusion user signed in on it.
	std::map<std::string, std::string> users;
	std::list<LocalCall> local_calls;
};

ParamInput CellOf(const JsonValue &value, const std::string &call) {
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
	default:
		throw ApiError("E_BAD_REQUEST", 422, call + ": a cell is a string, a number, a boolean or null");
	}
}

void AddValue(JsonArrayRef &array, const ExcelValue &value) {
	switch (value.kind) {
	case ExcelValue::Kind::STRING:
		array.AddString(value.text);
		break;
	case ExcelValue::Kind::NUMBER:
		array.AddDouble(value.number);
		break;
	case ExcelValue::Kind::BOOLEAN:
		array.AddBool(value.boolean);
		break;
	}
}

} // namespace

const std::string &CoreFunctionsJson() {
	static const std::string json(CORE_FUNCTIONS);
	return json;
}

std::vector<FormulaResult> EvaluateBatch(duckdb::DatabaseInstance &db, const BatchContext &context,
                                         const std::vector<FormulaCall> &calls) {
	return Evaluator(db, context).Run(calls);
}

JsonObject FormulaResultJson(const FormulaResult &result, bool with_number) {
	JsonObject out;
	if (with_number) {
		out.Set("i", result.i);
	}
	if (!result.ok) {
		JsonObject error;
		error.Set("code", result.error).Set("msg", result.message);
		out.Set("e", error);
		return out;
	}
	if (!result.matrix) {
		const auto &value = result.rows[0][0];
		switch (value.kind) {
		case ExcelValue::Kind::STRING:
			out.Set("v", value.text);
			break;
		case ExcelValue::Kind::NUMBER:
			out.SetDouble("v", value.number);
			break;
		case ExcelValue::Kind::BOOLEAN:
			out.Set("v", value.boolean);
			break;
		}
		return out;
	}
	auto rows = out.SetArray("v");
	for (const auto &row : result.rows) {
		auto cells = rows.AddArray();
		for (const auto &value : row) {
			AddValue(cells, value);
		}
	}
	return out;
}

std::vector<CellMatrix> FormulaArgs(const JsonValue &args, const std::string &call) {
	std::vector<CellMatrix> matrices;
	if (args.Is(JsonKind::MISSING) || args.Is(JsonKind::NULL_VALUE)) {
		return matrices;
	}
	const auto refuse = [&]() {
		return ApiError("E_BAD_REQUEST", 422, call + ": args must be an array of matrices (arrays of rows)");
	};
	if (!args.Is(JsonKind::ARRAY)) {
		throw refuse();
	}
	for (size_t a = 0; a < args.Size(); a++) {
		const auto matrix = args.At(a);
		if (!matrix.Is(JsonKind::ARRAY)) {
			throw refuse();
		}
		CellMatrix cells;
		for (size_t r = 0; r < matrix.Size(); r++) {
			const auto row = matrix.At(r);
			if (!row.Is(JsonKind::ARRAY)) {
				throw refuse();
			}
			cells.emplace_back();
			for (size_t c = 0; c < row.Size(); c++) {
				cells.back().push_back(CellOf(row.At(c), call));
			}
		}
		matrices.push_back(std::move(cells));
	}
	return matrices;
}

void ClearFormulaCache() {
	ResultCache::Get().Clear();
}

} // namespace ofexcel
