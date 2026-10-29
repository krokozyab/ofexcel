// Drives oracle_fusion_query through ofquack's transport seam with scripted SOAP
// responses. No Fusion instance, no network, no credentials.
//
// This pins the ofquack behaviour CONTRACTS.md §5 builds on. If one of these
// checks fails after an ofquack update, the contract needs revisiting before the
// code does:
//   - all_varchar returns the text dbms_xmlgen emitted, untouched (Q4);
//   - an empty result is an IOException starting "Oracle Fusion returned no rows" (§5.2);
//   - columns come back in SELECT order, named as the report named them.
//
// The fake follows ofquack's own test/cpp/adapter_test.cpp.

#include "test_support.hpp"

#include "base64.h"
#include "ofquack/transport.hpp"

#include <memory>
#include <string>
#include <vector>

using test_support::Contains;
using test_support::Run;
using test_support::RunOk;

namespace {

std::string MakeSoapResponse(const std::string &report_xml) {
	const auto encoded = base64_encode(reinterpret_cast<const unsigned char *>(report_xml.c_str()), report_xml.size());
	return "<soap:Envelope xmlns:soap=\"http://www.w3.org/2003/05/soap-envelope\"><soap:Body>"
	       "<ns2:runReportResponse><ns2:runReportReturn><ns2:reportBytes>" +
	       encoded + "</ns2:reportBytes></ns2:runReportReturn></ns2:runReportResponse></soap:Body></soap:Envelope>";
}

//! The report wraps the rowset as escaped XML text inside RESULT.
std::string MakeReportXML(const std::string &escaped_rowset) {
	return "<DATA_DS><G_1><RESULT>" + escaped_rowset + "</RESULT></G_1></DATA_DS>";
}

//! True when a paged statement asks for anything past the first page. A scan
//! ends on an empty page, so a fake that ignored the offset would never end.
bool AsksForALaterPage(const std::string &sql) {
	const auto at = sql.find("OFFSET ");
	return at != std::string::npos && sql.compare(at + 7, 1, "0") != 0;
}

struct Script {
	std::string first_page;
	std::vector<std::string> executed_sql;
};

class FakeTransport : public ofquack::FusionTransport {
public:
	explicit FakeTransport(Script &script_p) : script(script_p) {
	}

	std::string Execute(const std::string &sql, const ofquack::RequestContext &) override {
		script.executed_sql.push_back(sql);
		if (AsksForALaterPage(sql)) {
			return MakeSoapResponse(MakeReportXML("&lt;ROWSET/&gt;"));
		}
		return script.first_page;
	}

private:
	Script &script;
};

ofquack::ScopedTransportFactory InstallFake(Script &script) {
	return ofquack::ScopedTransportFactory(
	    [&script](const ofquack::FusionConfig &) { return std::make_shared<FakeTransport>(script); });
}

void CreateSecret(duckdb::Connection &connection) {
	RunOk(connection, "CREATE SECRET fusion (TYPE oracle_fusion, "
	                  "ENDPOINT 'https://fusion.example.com/xmlpserver/services/ExternalReportWSSService?WSDL', "
	                  "REPORT_PATH '/Custom/Financials/RP_ARB.xdo', USERNAME 'analyst', PASSWORD 'hunter2')");
}

//! The values dbms_xmlgen emits for an Oracle NUMBER below one, a negative
//! fraction, an identifier with leading zeros and a date.
const char *const TEXT_ROWS = "&lt;ROWSET&gt;"
                              "&lt;ROW&gt;&lt;AMT&gt;.84&lt;/AMT&gt;&lt;CODE&gt;0012&lt;/CODE&gt;"
                              "&lt;D&gt;2026-01-01&lt;/D&gt;&lt;/ROW&gt;"
                              "&lt;ROW&gt;&lt;AMT&gt;-1.5&lt;/AMT&gt;&lt;CODE&gt;13&lt;/CODE&gt;"
                              "&lt;D&gt;2026-02-01&lt;/D&gt;&lt;/ROW&gt;"
                              "&lt;/ROWSET&gt;";

void TestAllVarcharReturnsTheTextUntouched() {
	Script script;
	script.first_page = MakeSoapResponse(MakeReportXML(TEXT_ROWS));
	auto installed = InstallFake(script);

	test_support::TestDatabase db;
	duckdb::Connection connection(db);
	CreateSecret(connection);
	const auto result = RunOk(connection, "SELECT * FROM oracle_fusion_query("
	                                      "'SELECT amt, code, d FROM t', secret := 'fusion', all_varchar := true, "
	                                      "on_cast_error := 'error')");

	CHECK(result.names.size() == 3);
	CHECK(result.names[0] == "AMT");
	CHECK(result.names[1] == "CODE");
	CHECK(result.names[2] == "D");
	for (const auto &type : result.types) {
		CHECK(type.id() == duckdb::LogicalTypeId::VARCHAR);
	}
	CHECK(result.rows.size() == 2);
	CHECK(result.At(0, 0).ToString() == ".84");
	CHECK(result.At(1, 0).ToString() == "-1.5");
	CHECK(result.At(0, 1).ToString() == "0012");
	CHECK(result.At(0, 2).ToString() == "2026-01-01");
	CHECK(!script.executed_sql.empty());
}

void TestEmptyResultIsTheErrorTheContractRecognises() {
	Script script;
	script.first_page = MakeSoapResponse(MakeReportXML("&lt;ROWSET/&gt;"));
	auto installed = InstallFake(script);

	test_support::TestDatabase db;
	duckdb::Connection connection(db);
	CreateSecret(connection);
	const auto result = Run(connection, "SELECT * FROM oracle_fusion_query("
	                                    "'SELECT 1 x FROM dual WHERE 1 = 0', secret := 'fusion', all_varchar := true)");
	CHECK(result.HasError());
	CHECK(Contains(result.error, "Oracle Fusion returned no rows"));
}

} // namespace

int main() {
	TestAllVarcharReturnsTheTextUntouched();
	TestEmptyResultIsTheErrorTheContractRecognises();
	std::cout << "fusion_seam_test: all checks passed\n";
	return 0;
}
