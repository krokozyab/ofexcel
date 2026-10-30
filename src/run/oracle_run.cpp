// An Oracle run (CONTRACTS.md §5.1, §5.2, §3.3): the rendered SQL through
// ofquack into a stage table of VARCHAR columns, the types decided over the
// whole result, and the values moved into the new version.

#include "run_work.hpp"

#include "duckdb/main/appender.hpp"
#include "ofexcel/db_util.hpp"
#include "ofexcel/fusion_errors.hpp"
#include "ofexcel/result_types.hpp"

#include <functional>

namespace ofexcel {

namespace {

class OracleRun : public RunWork {
public:
	using RunWork::RunWork;

protected:
	void Fetch(Outcome &outcome) override {
		const auto error = ReadIntoStage(outcome);
		if (error.HasError()) {
			if (Stopping()) {
				throw Stopped();
			}
			if (!IsFusionNoRows(error)) {
				throw FusionApiError(error);
			}
			StoreEmpty(outcome);
			return;
		}
		ThrowIfStopping();
		StoreRows(outcome);
	}

	ApiError MapError(const duckdb::ErrorData &error) const override {
		return FusionApiError(error);
	}

private:
	//! §5.1: every column VARCHAR, as all_varchar returns them.
	duckdb::ErrorData ReadIntoStage(Outcome &outcome) {
		auto prepared = reader.Prepare(ORACLE_QUERY_SQL);
		if (prepared->HasError()) {
			return prepared->GetErrorObject();
		}
		duckdb::vector<duckdb::Value> values {duckdb::Value(plan.rendered_sql), duckdb::Value(plan.connection),
		                                      duckdb::Value::UBIGINT(static_cast<uint64_t>(plan.fetch_size))};
		return StreamIntoStage([&]() { return prepared->Submit(values); },
		                       [](const duckdb::LogicalType &) { return duckdb::LogicalType::VARCHAR; }, outcome);
	}

	//! The text of every value of `column`, "" for NULL: Oracle does not tell
	//! them apart, and ofquack sends NULL as "" anyway (§3.3).
	static void ForEachText(duckdb::DataChunk &chunk, duckdb::idx_t column,
	                        const std::function<void(duckdb::idx_t, const std::string &)> &visit) {
		duckdb::UnifiedVectorFormat format;
		chunk.data[column].ToUnifiedFormat(format);
		const auto *strings = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(format);
		const std::string empty;
		for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
			const auto index = format.sel->get_index(row);
			if (format.validity.RowIsValid(index)) {
				visit(row, strings[index].GetString());
			} else {
				visit(row, empty);
			}
		}
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

	static const SchemaColumn *Known(const std::vector<SchemaColumn> &known, const std::string &name) {
		const auto lower = Lower(name);
		for (const auto &column : known) {
			if (Lower(column.name) == lower) {
				return &column;
			}
		}
		return nullptr;
	}

	//! §3.3 over the whole stage table.
	std::vector<SchemaColumn> InferSchema(const std::vector<SchemaColumn> &known) {
		std::vector<OracleColumnInference> inference;
		for (const auto &name : names) {
			const auto *declared = Declared(name);
			const auto *start = Known(known, name);
			inference.emplace_back(name, declared ? &declared->type : nullptr, start ? &start->type : nullptr);
		}
		ScanStage([&](duckdb::DataChunk &chunk) {
			for (duckdb::idx_t column = 0; column < chunk.ColumnCount(); column++) {
				auto &column_inference = inference[column];
				ForEachText(chunk, column, [&](duckdb::idx_t, const std::string &text) { column_inference.Add(text); });
			}
		});

		std::vector<SchemaColumn> schema;
		for (size_t i = 0; i < names.size(); i++) {
			const auto finished = inference[i].Finish();
			if (!finished.ok) {
				throw ApiError(finished.code, 500, finished.message);
			}
			for (const auto &message : finished.warnings) {
				warnings.push_back({finished.widened ? WARNING_TYPE_WIDENED : WARNING_NUMBER_PRECISION, message});
			}
			const auto *declared = Declared(names[i]);
			schema.push_back({names[i], finished.type, declared ? declared->header : ""});
		}
		return schema;
	}

	void StoreRows(Outcome &outcome) {
		const auto schema = InferSchema(ReadQuerySchema(writer, plan.definition_hash));
		CreateTarget(schema, outcome);
		duckdb::Appender appender(writer, duckdb::Identifier(DATASET_SCHEMA), duckdb::Identifier(outcome.result_table));
		ScanStage([&](duckdb::DataChunk &chunk) {
			std::vector<std::vector<duckdb::Value>> rows(chunk.size(), std::vector<duckdb::Value>(schema.size()));
			for (duckdb::idx_t column = 0; column < chunk.ColumnCount(); column++) {
				const auto &type = schema[column].type;
				ForEachText(chunk, column, [&](duckdb::idx_t row, const std::string &text) {
					std::string stored;
					switch (ConvertOracleValue(type, text, stored)) {
					case StoredValue::VALUE:
						rows[row][column] = duckdb::Value(stored);
						break;
					case StoredValue::NULL_VALUE:
						break;
					case StoredValue::MISMATCH:
						// Inference saw every value, so this is a bug, not bad data.
						throw ApiError("E_INTERNAL", 500,
						               "value \"" + text + "\" does not fit type " + type.ToString() + " of column " +
						                   schema[column].name);
					}
				});
			}
			for (auto &row : rows) {
				appender.BeginRow();
				for (auto &value : row) {
					appender.Append(std::move(value));
				}
				appender.EndRow();
			}
		});
		appender.Close();
		ThrowIfStopping();
		Commit(outcome, schema, true);
	}

	//! §5.2: an empty result keeps the schema the query is known to have.
	void StoreEmpty(Outcome &outcome) {
		auto schema = ReadQuerySchema(writer, plan.definition_hash);
		for (auto &column : schema) {
			if (const auto *declared = Declared(column.name)) {
				column.type = declared->type;
				column.header = declared->header;
			}
		}
		if (schema.empty()) {
			for (const auto &declared : plan.declared) {
				schema.push_back({declared.name, declared.type, declared.header});
			}
		}
		if (schema.empty()) {
			throw ApiError("E_EMPTY_UNKNOWN_SCHEMA", 500,
			               "The query returned 0 rows and its schema is unknown. Declare the columns with @column, "
			               "or run the query with parameters that return rows");
		}
		outcome.rows = 0;
		outcome.truncated = false;
		CreateTarget(schema, outcome);
		ThrowIfStopping();
		Commit(outcome, schema, false);
	}
};

} // namespace

std::unique_ptr<RunWork> OracleWork(duckdb::DatabaseInstance &db, RunEngine::Active &run, const Plan &plan) {
	return std::unique_ptr<RunWork>(new OracleRun(db, run, plan));
}

} // namespace ofexcel
