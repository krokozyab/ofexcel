#pragma once

// Connections to Fusion (CONTRACTS.md §6.2 meta.connections, §8.3, §13;
// SPEC.md §3.1). A connection is an ofquack secret of TYPE oracle_fusion,
// PROVIDER browser, named like the connection. Secrets live in memory only, so
// meta.connections keeps the definitions -- never a credential -- and the
// secrets are recreated from it when the daemon starts.

#include "duckdb.hpp"

#include <string>
#include <vector>

namespace ofexcel {

//! How long fusion_scanner_sso_login() waits for the person to finish signing
//! in (CONTRACTS.md §8.3).
constexpr int64_t SSO_TIMEOUT_SECONDS = 300;

struct FusionConnection {
	std::string name;
	std::string environment; //!< DEV, TEST or PROD
	std::string endpoint;    //!< https://host[:port], no path
	std::string report_path;
};

bool IsEnvironment(const std::string &environment);

//! `definition` checked and normalised: the name per CONTRACTS.md §1, the
//! environment, an endpoint of https://host[:port] in lower case without a
//! path, an absolute report path. Throws ApiError E_BAD_REQUEST (422).
FusionConnection CheckConnection(FusionConnection definition);

//! "host[:port]" of a normalised endpoint.
std::string EndpointHost(const std::string &endpoint);

//! meta.connections, ordered by name.
std::vector<FusionConnection> ListConnections(duckdb::Connection &connection);

//! True when the secret `name` exists and is an oracle_fusion one, which is
//! what the routes and ofquack's functions can use.
bool FusionSecretExists(duckdb::Connection &connection, const std::string &name);

//! POST /v1/connections: the secret and the row together, or neither. Throws
//! ApiError E_CONFLICT (409) when a row or a secret already has the name, and
//! E_BAD_REQUEST as CheckConnection does. Returns the normalised definition.
FusionConnection CreateConnection(duckdb::Connection &connection, const FusionConnection &definition);

//! ofexcel_set_environment: sets the label of an existing row. A secret with
//! no row -- one init.sql created -- gets a row with the secret's endpoint and
//! report path, so init.sql can label its own connections. Throws ApiError
//! E_NOT_FOUND when neither exists, E_BAD_REQUEST for a bad environment.
FusionConnection SetConnectionEnvironment(duckdb::Connection &connection, const std::string &name,
                                          const std::string &environment);

//! At start, after init.sql: a secret for every row whose name no secret has
//! yet. init.sql's secret of the same name wins. Returns the names restored; a
//! row that cannot become a secret is logged to stderr and skipped, so one bad
//! row does not keep the daemon from starting.
std::vector<std::string> RestoreConnectionSecrets(duckdb::Connection &connection);

} // namespace ofexcel
