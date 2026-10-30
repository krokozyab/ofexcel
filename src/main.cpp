// ofexceld: the QExcel daemon. DuckDB with ofexcel, fusion_scanner and
// core_functions linked in, opened on one database.
//
//   ofexceld version
//   ofexceld serve [--db PATH] [--init FILE] [--port N] [--origin URL]
//   ofexceld sql   [--db PATH] [--init FILE] "SQL"
//   ofexceld install-cert   [--dir DIR] [--no-trust]
//   ofexceld uninstall-cert [--dir DIR] [--no-trust]
//
// `serve` opens QEXCEL_HOME/data.duckdb and runs QEXCEL_HOME/init.sql unless
// told otherwise, locks the configuration (src/daemon/startup.cpp) and runs
// until SIGINT or SIGTERM. Its stdout carries one line, "listening on <url>",
// for whatever started it; everything else goes to stderr. `sql` runs
// statements on an in-memory database unless --db says otherwise and prints
// the last result tab-separated, for scripts and smoke tests. `install-cert`
// creates the local CA and the server certificate the daemon serves HTTPS
// with, and trusts the CA for the current user; `uninstall-cert` undoes both.
// DIR defaults to QEXCEL_HOME/tls; --no-trust leaves the trust store alone.

#include "daemon/startup.hpp"
#include "duckdb.hpp"
#include "ofexcel/db_util.hpp"
#include "ofexcel/http_server.hpp"
#include "ofexcel/local_ca.hpp"
#include "ofexcel/paths.hpp"
#include "ofexcel/trust_store.hpp"
#include "ofexcel/version.hpp"

#include <csignal>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

struct Arguments {
	std::string command;
	//! "": serve takes QEXCEL_HOME/data.duckdb, sql an in-memory database.
	std::string db_path;
	std::string init_file;
	std::string port;
	std::string origin;
	std::string sql;
	std::string tls_dir;
	bool no_trust = false;
};

void PrintUsage() {
	std::cerr << "usage:\n"
	             "  ofexceld version\n"
	             "  ofexceld serve [--db PATH] [--init FILE] [--port N] [--origin URL]\n"
	             "  ofexceld sql   [--db PATH] [--init FILE] \"SQL\"\n"
	             "  ofexceld install-cert   [--dir DIR] [--no-trust]\n"
	             "  ofexceld uninstall-cert [--dir DIR] [--no-trust]\n";
}

bool ParseArguments(int argc, char **argv, Arguments &arguments) {
	if (argc < 2) {
		return false;
	}
	arguments.command = argv[1];
	for (int i = 2; i < argc; i++) {
		const std::string argument = argv[i];
		const auto take_value = [&](std::string &target) {
			if (i + 1 >= argc) {
				std::cerr << "ofexceld: " << argument << " needs a value\n";
				return false;
			}
			target = argv[++i];
			return true;
		};
		if (argument == "--db") {
			if (!take_value(arguments.db_path)) {
				return false;
			}
		} else if (argument == "--init") {
			if (!take_value(arguments.init_file)) {
				return false;
			}
		} else if (argument == "--port") {
			if (!take_value(arguments.port)) {
				return false;
			}
		} else if (argument == "--origin") {
			if (!take_value(arguments.origin)) {
				return false;
			}
		} else if (argument == "--dir") {
			if (!take_value(arguments.tls_dir)) {
				return false;
			}
		} else if (argument == "--no-trust") {
			arguments.no_trust = true;
		} else if (arguments.command == "sql" && arguments.sql.empty() && argument.rfind("--", 0) != 0) {
			arguments.sql = argument;
		} else {
			std::cerr << "ofexceld: unknown argument " << argument << "\n";
			return false;
		}
	}
	return true;
}

void HandleStopSignal(int) {
	ofexcel::HttpServer::RequestStop();
}

int Serve(const Arguments &arguments) {
	ofexcel::StartupOptions options;
	options.db_path = arguments.db_path;
	options.init_file = arguments.init_file;
	options.port = arguments.port;
	options.origin = arguments.origin;
	auto prepared = ofexcel::PrepareDaemonDatabase(options);
	std::cerr << "ofexceld " << ofexcel::VersionWithBuildStamp() << ": database " << prepared.db_path << ", init "
	          << (prepared.init_file.empty() ? "none" : prepared.init_file) << std::endl;

	std::signal(SIGINT, HandleStopSignal);
	std::signal(SIGTERM, HandleStopSignal);

	// ofexcel_start rather than ofexcel_serve, so the URL can be printed
	// before this thread blocks.
	duckdb::Connection connection(*prepared.db);
	auto started = connection.Query("SELECT url FROM ofexcel_start()");
	if (started->HasError()) {
		// The error as DuckDB phrased it, without QueryRows' wrapping.
		std::cerr << "ofexceld: " << started->GetError() << std::endl;
		return 1;
	}
	const auto chunk = started->Fetch();
	std::cout << "listening on " << chunk->GetValue(0, 0).ToString() << std::endl;
	ofexcel::HttpServer::Get().Wait();
	std::cerr << "ofexceld stopped" << std::endl;
	return 0;
}

int RunSql(const Arguments &arguments) {
	if (arguments.sql.empty()) {
		PrintUsage();
		return 2;
	}
	const auto db_path = arguments.db_path.empty() ? std::string(":memory:") : arguments.db_path;
	ofexcel::CheckDatabaseFileName(db_path);
	auto db = ofexcel::OpenDatabase(db_path);
	duckdb::Connection connection(*db);
	if (!arguments.init_file.empty()) {
		ofexcel::Execute(connection, ofexcel::ReadTextFile(arguments.init_file));
	}
	auto result = connection.Query(arguments.sql);
	if (result->HasError()) {
		std::cerr << result->GetError() << std::endl;
		return 1;
	}
	const auto &names = result->GetNames();
	for (size_t i = 0; i < names.size(); i++) {
		std::cout << (i ? "\t" : "") << names[i].GetIdentifierName();
	}
	std::cout << "\n";
	while (auto chunk = result->Fetch()) {
		if (chunk->size() == 0) {
			break;
		}
		for (duckdb::idx_t row = 0; row < chunk->size(); row++) {
			for (duckdb::idx_t column = 0; column < chunk->ColumnCount(); column++) {
				std::cout << (column ? "\t" : "") << chunk->GetValue(column, row).ToString();
			}
			std::cout << "\n";
		}
	}
	if (result->HasError()) {
		std::cerr << result->GetError() << std::endl;
		return 1;
	}
	return 0;
}

std::string TlsDirOf(const Arguments &arguments) {
	return arguments.tls_dir.empty() ? ofexcel::DefaultTlsDir() : arguments.tls_dir;
}

int InstallCert(const Arguments &arguments) {
	const auto files = ofexcel::TlsFilesIn(TlsDirOf(arguments));
	const auto result = ofexcel::InstallLocalCa(files.dir);
	std::cout << (result.ca_created ? "created " : "kept ") << files.ca_cert << "\n";
	std::cout << (result.server_issued ? "issued " : "kept ") << files.server_cert << "\n";
	if (arguments.no_trust) {
		std::cout << "trust store left alone (--no-trust)" << std::endl;
		return 0;
	}
	ofexcel::TrustCa(files.ca_cert, ofexcel::ReadCertificateIdentity(files.ca_cert));
	std::cout << "trusted " << files.ca_cert << " for the current user" << std::endl;
	return 0;
}

int UninstallCert(const Arguments &arguments) {
	namespace fs = std::filesystem;
	const auto files = ofexcel::TlsFilesIn(TlsDirOf(arguments));
	std::error_code error;
	const bool has_ca = fs::exists(fs::u8path(files.ca_cert), error);
	if (has_ca && !arguments.no_trust) {
		// Before the files go: the trust store is found by this certificate.
		ofexcel::UntrustCa(files.ca_cert, ofexcel::ReadCertificateIdentity(files.ca_cert));
		std::cout << "untrusted " << files.ca_cert << std::endl;
	}
	for (const auto &path : {files.ca_cert, files.ca_key, files.server_cert, files.server_key}) {
		if (fs::remove(fs::u8path(path), error)) {
			std::cout << "removed " << path << "\n";
		} else if (error) {
			throw std::runtime_error("Cannot remove " + path + ": " + error.message());
		}
	}
	// Only if nothing else is left in it.
	fs::remove(fs::u8path(files.dir), error);
	std::cout << std::flush;
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	Arguments arguments;
	if (!ParseArguments(argc, argv, arguments)) {
		PrintUsage();
		return 2;
	}
	try {
		if (arguments.command == "version") {
			std::cout << "ofexceld " << ofexcel::VersionWithBuildStamp() << std::endl;
			return 0;
		}
		if (arguments.command == "serve") {
			return Serve(arguments);
		}
		if (arguments.command == "sql") {
			return RunSql(arguments);
		}
		if (arguments.command == "install-cert") {
			return InstallCert(arguments);
		}
		if (arguments.command == "uninstall-cert") {
			return UninstallCert(arguments);
		}
	} catch (const std::exception &exception) {
		std::cerr << "ofexceld: " << duckdb::ErrorData(exception).Message() << std::endl;
		return 1;
	}
	PrintUsage();
	return 2;
}
