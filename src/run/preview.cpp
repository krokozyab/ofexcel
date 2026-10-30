// Preview (CONTRACTS.md §8.3): the query read as a run reads it -- through
// ofquack with a page of 200 rows (§5.1), or in DuckDB over the dependencies'
// current data (§5.3) -- but only as far as limit + 1 rows, held in memory,
// typed by §3.3 or §3.4 over those rows, and never stored.

#include "ofexcel/preview.hpp"

#include "ofexcel/api_error.hpp"
#include "ofexcel/connections.hpp"
#include "ofexcel/datasets.hpp"
#include "ofexcel/fusion_errors.hpp"
#include "ofexcel/hashing.hpp"
#include "ofexcel/oracle_render.hpp"
#include "ofexcel/query_file.hpp"
#include "ofexcel/result_types.hpp"
#include "run_work.hpp"

#include <chrono>

namespace ofexcel {

namespace {

const QueryColumn *Declared(const std::vector<QueryColumn> &declared, const std::string &name) {
	for (const auto &column : declared) {
		if (Lower(column.name) == Lower(name)) {
			return &column;
		}
	}
	return nullptr;
}

PreviewColumn Column(const std::string &name, const ColumnType &type, const QueryColumn *declared) {
	return {name, declared && !declared->header.empty() ? declared->header : name, type};
}

void PreviewOracle(duckdb::DatabaseInstance &db, duckdb::Connection &connection, const QueryHeader &header,
                   const PreviewRequest &request, WatchedReader &watched, PreviewResult &result) {
	const auto connection_name = request.connection.empty() ? header.connection : request.connection;
	if (connection_name.empty()) {
		throw ApiError("E_BAD_REQUEST", 422,
		               "No connection given: pass connection or declare @connection in query " + header.id);
	}
	if (!FusionSecretExists(connection, connection_name)) {
		throw ApiError("E_NOT_FOUND", 404, "No connection " + connection_name);
	}
	const auto rendered = RenderOracle(header.body, header.params, request.params);
	if (!rendered.ok) {
		throw ApiError(rendered.code, 422, rendered.message);
	}
	result.oracle = true;
	result.rendered_sql = rendered.rendered;
	for (const auto &message : rendered.warnings) {
		result.warnings.push_back({WARNING_PARAM_UNUSED, message});
	}

	duckdb::Connection reader(db);
	auto prepared = reader.Prepare(ORACLE_QUERY_SQL);
	if (prepared->HasError()) {
		throw FusionApiError(prepared->GetErrorObject());
	}
	duckdb::vector<duckdb::Value> values {duckdb::Value(rendered.rendered), duckdb::Value(connection_name),
	                                      duckdb::Value::UBIGINT(static_cast<uint64_t>(PREVIEW_MAX_ROWS))};
	const auto read = watched.Read(reader, request.limit, [&]() { return prepared->Submit(values); });
	watched.ThrowIfTimedOut();
	result.truncated = read.truncated;

	// §3.3 item 2: the schema the query's runs left is where the types start;
	// Preview reads it but does not write it.
	const auto known = ReadQuerySchema(connection, DefinitionHash(request.text));
	if (read.error.HasError()) {
		if (!IsFusionNoRows(read.error)) {
			throw FusionApiError(read.error);
		}
		// §5.2's schema for no rows, or none: Preview is not a run, so an
		// unknown schema is no error.
		for (const auto &column : known) {
			const auto *declared = Declared(header.columns, column.name);
			result.columns.push_back(Column(column.name, declared ? declared->type : column.type, declared));
		}
		if (result.columns.empty()) {
			for (const auto &declared : header.columns) {
				result.columns.push_back(Column(declared.name, declared.type, &declared));
			}
		}
		return;
	}

	std::vector<OracleColumnInference> inference;
	for (const auto &name : read.names) {
		const auto *declared = Declared(header.columns, name);
		const SchemaColumn *start = nullptr;
		for (const auto &column : known) {
			if (Lower(column.name) == Lower(name)) {
				start = &column;
			}
		}
		inference.emplace_back(name, declared ? &declared->type : nullptr, start ? &start->type : nullptr);
	}
	// "" for NULL: Oracle does not tell them apart (§3.3).
	const auto text = [](const duckdb::Value &value) { return value.IsNull() ? std::string() : value.ToString(); };
	for (const auto &row : read.rows) {
		for (size_t i = 0; i < row.size(); i++) {
			inference[i].Add(text(row[i]));
		}
	}
	for (size_t i = 0; i < read.names.size(); i++) {
		const auto finished = inference[i].Finish();
		if (!finished.ok) {
			throw ApiError(finished.code, 422, finished.message);
		}
		for (const auto &message : finished.warnings) {
			result.warnings.push_back({finished.widened ? WARNING_TYPE_WIDENED : WARNING_NUMBER_PRECISION, message});
		}
		result.columns.push_back(Column(read.names[i], finished.type, Declared(header.columns, read.names[i])));
	}
	for (const auto &row : read.rows) {
		std::vector<duckdb::Value> stored_row;
		for (size_t i = 0; i < row.size(); i++) {
			const auto &type = result.columns[i].type;
			const auto storage = StorageType(type);
			std::string stored;
			switch (ConvertOracleValue(type, text(row[i]), stored)) {
			case StoredValue::VALUE:
				stored_row.push_back(duckdb::Value(stored).DefaultCastAs(storage));
				break;
			case StoredValue::NULL_VALUE:
				stored_row.push_back(duckdb::Value(storage));
				break;
			case StoredValue::MISMATCH:
				// Inference saw every value, so this is a bug, not bad data.
				throw ApiError("E_INTERNAL", 500,
				               "value \"" + text(row[i]) + "\" does not fit type " + type.ToString() + " of column " +
				                   read.names[i]);
			}
		}
		result.rows.push_back(std::move(stored_row));
	}
}

void PreviewLocal(duckdb::DatabaseInstance &db, duckdb::Connection &connection, const QueryHeader &header,
                  const PreviewRequest &request, WatchedReader &watched, PreviewResult &result) {
	// Dependencies are read as they are: Preview runs none of them, and records
	// nothing in meta.missing (no workbook).
	RunRequest run;
	run.params = request.params;
	run.connection = request.connection;
	run.deps = DepsMode::NONE;
	Plan plan;
	PlanLocalRun(connection, header, run, plan);
	const auto rows = LocalEvaluation(db, connection, header).Run(plan, request.limit, watched);
	for (const auto &warning : rows.warnings) {
		result.warnings.push_back({warning.code, warning.message});
	}
	for (const auto &column : rows.columns) {
		result.columns.push_back({column.name, column.header.empty() ? column.name : column.header, column.type});
	}
	result.rows = rows.rows;
	result.truncated = rows.truncated;
}

} // namespace

PreviewResult RunPreview(duckdb::DatabaseInstance &db, const PreviewRequest &request) {
	const auto started = std::chrono::steady_clock::now();
	const auto parsed = ParseQueryFile(request.text);
	if (!parsed.Ok()) {
		throw ApiError(parsed.errors[0].code, 422, "The query cannot be parsed: " + parsed.errors[0].ToString());
	}
	const auto &header = parsed.header;
	duckdb::Connection connection(db);
	PreviewResult result;
	{
		WatchedReader watched(RunTimeoutSetting(connection));
		if (header.engine == Engine::LOCAL) {
			PreviewLocal(db, connection, header, request, watched, result);
		} else {
			PreviewOracle(db, connection, header, request, watched, result);
		}
	}
	result.elapsed_ms =
	    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
	return result;
}

} // namespace ofexcel
