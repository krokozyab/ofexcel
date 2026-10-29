// ofexcel_live_probe: checks against a real Fusion instance that the automated
// tests cannot make. Not registered with ctest: it needs an SSO sign-in and a
// network. Phase 0 (SPEC.md §16) uses it; results go to docs/PHASE0_REPORT.md.
//
//   FUSION_ENDPOINT=https://fa-xxxx.fa.ocs.oraclecloud.com \
//   FUSION_REPORT_PATH=/Custom/CloudSQL/RP_ARB.xdo \
//   ofexcel_live_probe cancel [delay_seconds]
//
// `cancel` answers CONTRACTS.md A4 / Q2: does Connection::Interrupt() stop a
// request that is in flight, and is the connection usable afterwards? It runs
// two variants: one request for the whole result (fetch_size := 0), where an
// interrupt can only land mid-request, and a paged one.

#include "test_support.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

namespace {

using Clock = std::chrono::steady_clock;

double SecondsSince(Clock::time_point start) {
	return std::chrono::duration<double>(Clock::now() - start).count();
}

std::string RequiredEnv(const char *name) {
	const char *value = std::getenv(name);
	if (!value || !*value) {
		std::cerr << name << " is not set\n";
		std::exit(2);
	}
	return value;
}

const char *const BIG_QUERY =
    "SELECT invoice_id, line_number, line_type_lookup_code, amount, description, accounting_date, period_name, "
    "org_id, creation_date, last_update_date, quantity_invoiced, unit_price, po_header_id, base_amount "
    "FROM ap_invoice_lines_all ORDER BY invoice_id, line_number FETCH FIRST 300000 ROWS ONLY";

void CancelVariant(duckdb::Connection &connection, const char *label, int fetch_size, int delay_seconds) {
	const std::string sql = std::string("CREATE OR REPLACE TABLE probe_big AS SELECT * FROM oracle_fusion_query('") +
	                        BIG_QUERY + "', secret := 'fusion', all_varchar := true, fetch_size := " +
	                        std::to_string(fetch_size) + ")";
	test_support::Result result;
	const auto started = Clock::now();
	Clock::time_point finished;
	std::thread runner([&]() {
		result = test_support::Run(connection, sql);
		finished = Clock::now();
	});

	std::this_thread::sleep_for(std::chrono::seconds(delay_seconds));
	const auto interrupted_at = Clock::now();
	connection.Interrupt();
	runner.join();

	const auto stop_latency = std::chrono::duration<double>(finished - interrupted_at).count();
	std::cout << "[" << label << "] interrupt after " << delay_seconds << " s; query returned "
	          << stop_latency << " s later (total " << SecondsSince(started) << " s)\n";
	std::cout << "[" << label << "] error: " << (result.HasError() ? result.error : "(none: query completed)")
	          << "\n";

	// The connection, and ofquack's BI Publisher session behind it, must still work.
	const auto after = Clock::now();
	const auto check = test_support::Run(
	    connection, "SELECT * FROM oracle_fusion_query('SELECT 1 AS x FROM dual', secret := 'fusion', all_varchar := true)");
	std::cout << "[" << label << "] next query on the same connection: "
	          << (check.HasError() ? "FAILED: " + check.error : "ok") << " (" << SecondsSince(after) << " s)\n";
}

int Cancel(int delay_seconds) {
	test_support::TestDatabase db;
	duckdb::Connection connection(db);
	test_support::RunOk(connection, "CREATE SECRET fusion (TYPE oracle_fusion, PROVIDER browser, ENDPOINT '" +
	                                    RequiredEnv("FUSION_ENDPOINT") + "', REPORT_PATH '" +
	                                    RequiredEnv("FUSION_REPORT_PATH") + "')");
	const auto login_started = Clock::now();
	test_support::RunOk(connection, "SELECT * FROM fusion_scanner_sso_login()");
	std::cout << "signed in (" << SecondsSince(login_started) << " s)\n";

	CancelVariant(connection, "single request", 0, delay_seconds);
	CancelVariant(connection, "paged 5000", 5000, delay_seconds);
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	const std::string command = argc > 1 ? argv[1] : "";
	if (command == "cancel") {
		return Cancel(argc > 2 ? std::atoi(argv[2]) : 10);
	}
	std::cerr << "usage: ofexcel_live_probe cancel [delay_seconds]\n";
	return 2;
}
