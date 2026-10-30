// A Local query evaluated outside a run (CONTRACTS.md §5.3, §3.4): for
// Preview and for formulas. Read into memory as far as the caller's limit,
// typed by §3.4 over those rows, values cast as a run would store them.

#include "run_work.hpp"

#include "ofexcel/local_types.hpp"

namespace ofexcel {

namespace {

//! An integer's value has 16 digits or more: |value| >= 10^15 (§3.4).
bool FromId(const duckdb::Value &value) {
	if (value.IsNull()) {
		return false;
	}
	const auto text = value.ToString();
	return text.size() - (text[0] == '-' ? 1 : 0) >= 16;
}

const QueryColumn *Declared(const std::vector<QueryColumn> &declared, const std::string &name) {
	for (const auto &column : declared) {
		if (Lower(column.name) == Lower(name)) {
			return &column;
		}
	}
	return nullptr;
}

} // namespace

LocalEvaluation::LocalEvaluation(duckdb::DatabaseInstance &db, duckdb::Connection &meta_p, const QueryHeader &header_p)
    : meta(meta_p), header(header_p), reader(db) {
}

LocalRows LocalEvaluation::Run(const Plan &plan, int64_t limit, WatchedReader &watched) {
	CreateDependencyViews(reader, meta, plan.depends);
	// Prepared once: DuckDB binds it again by itself when the views change.
	if (!prepared) {
		prepared = reader.Prepare(plan.body);
	}
	if (prepared->HasError()) {
		throw LocalSqlError(prepared->GetErrorObject());
	}
	auto named = BindLocalParams(*prepared, plan);
	const auto read = watched.Read(reader, limit, [&]() { return prepared->Submit(named); });
	watched.ThrowIfTimedOut();
	if (read.error.HasError()) {
		throw LocalSqlError(read.error);
	}

	LocalRows result;
	result.truncated = read.truncated;
	result.warnings = plan.warnings;
	for (size_t i = 0; i < read.names.size(); i++) {
		for (size_t j = 0; j < i; j++) {
			if (Lower(read.names[i]) == Lower(read.names[j])) {
				throw ApiError("E_SQL_COMPILE", 422,
				               "The result has two columns named " + read.names[i] + ": give them different names");
			}
		}
	}
	std::vector<const QueryColumn *> declared;
	for (size_t i = 0; i < read.names.size(); i++) {
		declared.push_back(Declared(header.columns, read.names[i]));
		if (declared.back()) {
			result.columns.push_back({read.names[i], declared.back()->type, declared.back()->header});
			continue;
		}
		LocalColumnStats stats;
		if (IsIntegerType(read.types[i])) {
			for (const auto &row : read.rows) {
				stats.has_value_from_1e15 = stats.has_value_from_1e15 || FromId(row[i]);
			}
		}
		const auto mapping = MapLocalType(read.types[i], read.names[i], stats);
		if (mapping.cast_to_varchar) {
			result.warnings.push_back({WARNING_TYPE_WIDENED, mapping.warning});
		}
		result.columns.push_back({read.names[i], mapping.type, ""});
	}
	for (size_t r = 0; r < read.rows.size(); r++) {
		std::vector<duckdb::Value> stored_row;
		for (size_t i = 0; i < read.names.size(); i++) {
			const auto storage = StorageType(result.columns[i].type);
			const auto &value = read.rows[r][i];
			if (declared[i]) {
				stored_row.push_back(
				    DeclaredLocalValue(read.names[i], *declared[i], value, static_cast<int64_t>(r) + 1)
				        .DefaultCastAs(storage));
			} else {
				// As a run stores it: the stage type first, then the storage type.
				stored_row.push_back(value.DefaultCastAs(LocalStageType(read.types[i])).DefaultCastAs(storage));
			}
		}
		result.rows.push_back(std::move(stored_row));
	}
	return result;
}

} // namespace ofexcel
