#pragma once

// Decides whether an HTTP request may reach the API. CONTRACTS.md §8.1.
//
// Pure: no DuckDB, no httplib. Every rule here has a vector in
// test/cpp/guard_test.cpp; change the vectors and CONTRACTS.md together.

#include <string>

namespace ofexcel {

enum class Profile {
	HOSTED, //!< add-in served from an HTTPS host, API over https://127.0.0.1 with CORS
	LOCAL,  //!< add-in and API both served by the daemon over https://localhost
};

struct GuardConfig {
	Profile profile = Profile::HOSTED;
	//! The port actually bound; Host must name it.
	int port = 0;
	//! HOSTED: the only Origin allowed, as returned by NormalizeHostedOrigin.
	std::string hosted_origin;
	//! LOCAL: the value X-OF-Session must carry.
	std::string session_token;
};

//! The headers the guard looks at. `has_*` distinguishes an absent header from
//! an empty one.
struct RequestHeaders {
	std::string host;
	bool has_origin = false;
	std::string origin;
	bool has_session = false;
	std::string session;
};

enum class GuardVerdict {
	ALLOW,
	BAD_HOST,    //!< 403 E_HOST
	BAD_ORIGIN,  //!< 403 E_ORIGIN, and no CORS headers
	BAD_SESSION, //!< 401 E_SESSION
};

//! For every /v1 request other than a CORS preflight.
GuardVerdict CheckApiRequest(const GuardConfig &config, const RequestHeaders &headers);

//! For an OPTIONS preflight, which never carries X-OF-Session.
GuardVerdict CheckPreflight(const GuardConfig &config, const RequestHeaders &headers);

//! `https://host[:port]` in lower case, or "" if `origin` is not one. A path,
//! query, user info, trailing slash or any other scheme makes it invalid.
std::string NormalizeHostedOrigin(const std::string &origin);

//! The error code a verdict maps to, e.g. "E_ORIGIN". "" for ALLOW.
const char *VerdictCode(GuardVerdict verdict);

} // namespace ofexcel
