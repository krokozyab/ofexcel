// ofexceld: the QExcel daemon. DuckDB with ofexcel, fusion_scanner and
// core_functions linked in, opened on one database.
//
//   ofexceld version
//   ofexceld serve [--db PATH] [--init FILE] [--port N] [--origin URL]
//   ofexceld sql   [--db PATH] [--init FILE] "SQL"
//   ofexceld install-cert   [--dir DIR] [--no-trust]
//   ofexceld uninstall-cert [--dir DIR] [--no-trust]
//
// `serve` runs until SIGINT or SIGTERM. `sql` runs statements and prints the
// last result tab-separated, for scripts and smoke tests. `install-cert`
// creates the local CA and the server certificate the daemon serves HTTPS
// with, and trusts the CA for the current user; `uninstall-cert` undoes both.
// DIR defaults to QEXCEL_HOME/tls; --no-trust leaves the trust store alone.

#include "core_functions_extension.hpp"
#include "duckdb.hpp"
#include "fusion_scanner_extension.hpp"
#include "ofexcel/db_util.hpp"
#include "ofexcel/http_server.hpp"
#include "ofexcel/local_ca.hpp"
#include "ofexcel/paths.hpp"
#include "ofexcel/trust_store.hpp"
#include "ofexcel/version.hpp"
#include "ofexcel_extension.hpp"

#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct Arguments {
	std::string command;
	std::string db_path = ":memory:";
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

std::string ReadFile(const std::string &path) {
	std::ifstream file(path, std::ios::binary);
	if (!file) {
		throw std::runtime_error("cannot read " + path);
	}
	std::ostringstream content;
	content << file.rdbuf();
	return content.str();
}

//! Quotes a value for use as a SQL string literal.
std::string SqlString(const std::string &value) {
	std::string quoted = "'";
	for (const auto c : value) {
		quoted += c;
		if (c == '\'') {
			quoted += '\'';
		}
	}
	return quoted + "'";
}

//! Opens the database with every extension the daemon needs. Nothing is
//! autoloaded or autoinstalled: what this binary can do is what is linked in.
duckdb::unique_ptr<duckdb::DuckDB> OpenDatabase(const std::string &path) {
	duckdb::DBConfig config;
	config.SetOptionByName("autoinstall_known_extensions", duckdb::Value::BOOLEAN(false));
	config.SetOptionByName("autoload_known_extensions", duckdb::Value::BOOLEAN(false));
	auto db = duckdb::make_uniq<duckdb::DuckDB>(path == ":memory:" ? nullptr : path.c_str(), &config);
	db->LoadStaticExtension<duckdb::CoreFunctionsExtension>();
	db->LoadStaticExtension<duckdb::FusionScannerExtension>();
	db->LoadStaticExtension<duckdb::OfexcelExtension>();
	return db;
}

void RunInitFile(duckdb::Connection &connection, const std::string &path) {
	if (!path.empty()) {
		ofexcel::Execute(connection, ReadFile(path));
	}
}

//! The settings SPEC.md §8.4 asks for once the database is configured. After
//! lock_configuration nothing, including init.sql, can change a setting.
void Harden(duckdb::Connection &connection) {
	ofexcel::Execute(connection, "SET allow_community_extensions = false");
	ofexcel::Execute(connection, "SET enable_external_access = false");
	ofexcel::Execute(connection, "SET lock_configuration = true");
}

void HandleStopSignal(int) {
	ofexcel::HttpServer::RequestStop();
}

int Serve(const Arguments &arguments) {
	auto db = OpenDatabase(arguments.db_path);
	duckdb::Connection connection(*db);
	RunInitFile(connection, arguments.init_file);
	if (!arguments.port.empty()) {
		ofexcel::Execute(connection, "SET ofexcel_port = " + std::to_string(std::stoi(arguments.port)));
	}
	if (!arguments.origin.empty()) {
		ofexcel::Execute(connection, "SET ofexcel_hosted_origin = " + SqlString(arguments.origin));
	}
	Harden(connection);

	std::signal(SIGINT, HandleStopSignal);
	std::signal(SIGTERM, HandleStopSignal);

	// ofexcel_start rather than ofexcel_serve, so the URL can be printed
	// before this thread blocks.
	const auto rows = ofexcel::QueryRows(connection, "SELECT url FROM ofexcel_start()");
	std::cout << "ofexceld " << ofexcel::VersionWithBuildStamp() << " listening on " << rows.at(0).at(0).ToString()
	          << std::endl;
	ofexcel::HttpServer::Get().Wait();
	std::cout << "ofexceld stopped" << std::endl;
	return 0;
}

int RunSql(const Arguments &arguments) {
	if (arguments.sql.empty()) {
		PrintUsage();
		return 2;
	}
	auto db = OpenDatabase(arguments.db_path);
	duckdb::Connection connection(*db);
	RunInitFile(connection, arguments.init_file);
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
			throw std::runtime_error("Не удалось удалить " + path + ": " + error.message());
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
