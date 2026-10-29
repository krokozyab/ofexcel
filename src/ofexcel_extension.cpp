#define DUCKDB_EXTENSION_MAIN

#include "ofexcel_extension.hpp"

#include "duckdb.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "ofexcel/sql_functions.hpp"
#include "ofexcel/version.hpp"

namespace duckdb {

static void LoadInternal(ExtensionLoader &loader) {
	loader.SetDescription("QExcel: Oracle Fusion queries, local datasets and the Excel add-in API");
	ofexcel::RegisterSettings(loader);
	ofexcel::RegisterSqlFunctions(loader);
}

void OfexcelExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string OfexcelExtension::Name() {
	return "ofexcel";
}

std::string OfexcelExtension::Version() const {
	return ofexcel::PRODUCT_VERSION;
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(ofexcel, loader) {
	duckdb::LoadInternal(loader);
}

} // extern "C"
