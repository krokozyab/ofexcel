#include "ofexcel/connections.hpp"

#include "duckdb/catalog/catalog_transaction.hpp"
#include "duckdb/main/secret/secret_manager.hpp"
#include "ofexcel/api_error.hpp"
#include "ofexcel/db_util.hpp"
#include "ofexcel/query_file.hpp"
#include "ofexcel/request_guard.hpp"
#include "ofexcel/utf8.hpp"

#include <iostream>
#include <mutex>

namespace ofexcel {

namespace {

constexpr const char *SECRET_TYPE = "oracle_fusion";
constexpr size_t MAX_REPORT_PATH_BYTES = 1024;

//! Creating, labelling and restoring are check-then-write: two requests for one
//! name must not both pass the check. Process-wide, as the secrets are.
std::mutex &ConnectionsLock() {
	static std::mutex lock;
	return lock;
}

ApiError BadRequest(const std::string &message) {
	return ApiError("E_BAD_REQUEST", 422, message);
}

//! Runs `body` in a transaction on `connection`: the secret manager needs one,
//! and the secret and the row then commit or roll back together.
template <class BODY>
auto InTransaction(duckdb::Connection &connection, BODY body) -> decltype(body()) {
	connection.BeginTransaction();
	try {
		auto result = body();
		connection.Commit();
		return result;
	} catch (...) {
		if (connection.HasActiveTransaction()) {
			connection.Rollback();
		}
		throw;
	}
}

duckdb::unique_ptr<duckdb::SecretEntry> FindSecret(duckdb::Connection &connection, const std::string &name) {
	auto &context = *connection.context;
	auto transaction = duckdb::CatalogTransaction::GetSystemCatalogTransaction(context);
	return duckdb::SecretManager::Get(context).GetSecretByName(transaction, name);
}

bool HasRow(duckdb::Connection &connection, const std::string &name) {
	return !QueryRows(connection, "SELECT 1 FROM meta.connections WHERE name = $1", {duckdb::Value(name)}).empty();
}

void CreateBrowserSecret(duckdb::Connection &connection, const FusionConnection &definition) {
	duckdb::CreateSecretInput input;
	input.type = duckdb::Identifier(SECRET_TYPE);
	input.provider = duckdb::Identifier("browser");
	input.name = duckdb::Identifier(definition.name);
	input.options["endpoint"] = duckdb::Value(definition.endpoint);
	input.options["report_path"] = duckdb::Value(definition.report_path);
	input.options["sso_timeout_seconds"] = duckdb::Value::UBIGINT(SSO_TIMEOUT_SECONDS);
	input.on_conflict = duckdb::OnCreateConflict::ERROR_ON_CONFLICT;
	// In memory only: CREATE PERSISTENT SECRET writes secrets to disk in the
	// clear (SPEC.md §3.1). meta.connections is what survives a restart.
	input.persist_type = duckdb::SecretPersistType::TEMPORARY;
	auto &context = *connection.context;
	duckdb::SecretManager::Get(context).CreateSecret(context, input);
}

std::string SecretString(const duckdb::KeyValueSecret &secret, const char *key) {
	const auto value = secret.TryGetValue(duckdb::Identifier(key));
	return value.IsNull() ? "" : value.ToString();
}

//! scheme://authority of a URL such as ofquack's WSDL endpoint.
std::string OriginOf(const std::string &url) {
	const auto scheme = url.find("://");
	if (scheme == std::string::npos) {
		return url;
	}
	return url.substr(0, url.find_first_of("/?#", scheme + 3));
}

FusionConnection RowToConnection(const Row &row) {
	FusionConnection connection;
	connection.name = row[0].ToString();
	connection.environment = row[1].ToString();
	connection.endpoint = row[2].ToString();
	connection.report_path = row[3].ToString();
	return connection;
}

} // namespace

bool IsEnvironment(const std::string &environment) {
	return environment == "DEV" || environment == "TEST" || environment == "PROD";
}

FusionConnection CheckConnection(FusionConnection definition) {
	if (!IsIdentifier(definition.name)) {
		throw BadRequest("Имя подключения должно соответствовать ^[a-z][a-z0-9_]{0,62}$, например fusion_test");
	}
	if (!IsEnvironment(definition.environment)) {
		throw BadRequest("Окружение подключения должно быть DEV, TEST или PROD");
	}
	// The same rules as the hosted origin: https, a host, an optional port, no
	// path. ofquack adds the service path itself.
	const auto endpoint = NormalizeHostedOrigin(definition.endpoint);
	if (endpoint.empty()) {
		throw BadRequest("Адрес Fusion должен иметь вид https://host без пути, например "
		                 "https://fa-xxxx.oraclecloud.com");
	}
	definition.endpoint = endpoint;
	const auto &path = definition.report_path;
	bool printable = IsValidUtf8(path);
	for (const auto c : path) {
		if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) {
			printable = false;
		}
	}
	if (path.empty() || path[0] != '/' || path.size() > MAX_REPORT_PATH_BYTES || !printable) {
		throw BadRequest("Путь отчёта должен быть абсолютным путём в каталоге BI Publisher, например "
		                 "/Custom/Financials/RP_ARB.xdo");
	}
	return definition;
}

std::string EndpointHost(const std::string &endpoint) {
	const auto scheme = endpoint.find("://");
	return scheme == std::string::npos ? endpoint : endpoint.substr(scheme + 3);
}

std::vector<FusionConnection> ListConnections(duckdb::Connection &connection) {
	std::vector<FusionConnection> connections;
	for (const auto &row : QueryRows(connection, "SELECT name, environment, endpoint, report_path "
	                                             "FROM meta.connections ORDER BY name")) {
		connections.push_back(RowToConnection(row));
	}
	return connections;
}

bool FusionSecretExists(duckdb::Connection &connection, const std::string &name) {
	return InTransaction(connection, [&]() {
		const auto entry = FindSecret(connection, name);
		return entry && entry->secret->GetType() == SECRET_TYPE;
	});
}

FusionConnection CreateConnection(duckdb::Connection &connection, const FusionConnection &definition) {
	const auto checked = CheckConnection(definition);
	std::lock_guard<std::mutex> guard(ConnectionsLock());
	return InTransaction(connection, [&]() {
		if (HasRow(connection, checked.name) || FindSecret(connection, checked.name)) {
			throw ApiError("E_CONFLICT", 409, "Подключение " + checked.name + " уже есть");
		}
		CreateBrowserSecret(connection, checked);
		QueryRows(connection, "INSERT INTO meta.connections VALUES ($1, $2, $3, $4, $5)",
		          {duckdb::Value(checked.name), duckdb::Value(checked.environment), duckdb::Value(checked.endpoint),
		           duckdb::Value(checked.report_path),
		           duckdb::Value::TIMESTAMP(duckdb::Timestamp::GetCurrentTimestamp())});
		return checked;
	});
}

FusionConnection SetConnectionEnvironment(duckdb::Connection &connection, const std::string &name,
                                          const std::string &environment) {
	if (!IsEnvironment(environment)) {
		throw BadRequest("Окружение подключения должно быть DEV, TEST или PROD");
	}
	std::lock_guard<std::mutex> guard(ConnectionsLock());
	return InTransaction(connection, [&]() {
		const auto rows = QueryRows(connection,
		                            "UPDATE meta.connections SET environment = $2 WHERE name = $1 "
		                            "RETURNING name, environment, endpoint, report_path",
		                            {duckdb::Value(name), duckdb::Value(environment)});
		if (!rows.empty()) {
			return RowToConnection(rows[0]);
		}
		const auto entry = FindSecret(connection, name);
		const auto *secret =
		    entry && entry->secret->GetType() == SECRET_TYPE
		        ? dynamic_cast<const duckdb::KeyValueSecret *>(entry->secret.get())
		        : nullptr;
		if (!secret) {
			throw ApiError("E_NOT_FOUND", 404, "Нет подключения " + name);
		}
		FusionConnection definition;
		definition.name = name;
		definition.environment = environment;
		definition.endpoint = OriginOf(SecretString(*secret, "endpoint"));
		definition.report_path = SecretString(*secret, "report_path");
		const auto checked = CheckConnection(definition);
		QueryRows(connection, "INSERT INTO meta.connections VALUES ($1, $2, $3, $4, $5)",
		          {duckdb::Value(checked.name), duckdb::Value(checked.environment), duckdb::Value(checked.endpoint),
		           duckdb::Value(checked.report_path),
		           duckdb::Value::TIMESTAMP(duckdb::Timestamp::GetCurrentTimestamp())});
		return checked;
	});
}

std::vector<std::string> RestoreConnectionSecrets(duckdb::Connection &connection) {
	std::lock_guard<std::mutex> guard(ConnectionsLock());
	std::vector<std::string> restored;
	for (const auto &definition : ListConnections(connection)) {
		try {
			const bool created = InTransaction(connection, [&]() {
				if (FindSecret(connection, definition.name)) {
					return false;
				}
				CreateBrowserSecret(connection, CheckConnection(definition));
				return true;
			});
			if (created) {
				restored.push_back(definition.name);
			}
		} catch (const std::exception &error) {
			std::cerr << "ofexceld: connection " << definition.name
			          << " not restored: " << duckdb::ErrorData(error).Message() << std::endl;
		}
	}
	return restored;
}

} // namespace ofexcel
