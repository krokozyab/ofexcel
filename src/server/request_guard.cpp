#include "ofexcel/request_guard.hpp"

#include <cctype>
#include <cstddef>

namespace ofexcel {

namespace {

std::string ToLower(const std::string &text) {
	std::string lowered = text;
	for (auto &c : lowered) {
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	return lowered;
}

//! Compares without stopping at the first difference, so the time taken does
//! not reveal how much of a guessed session token was right.
bool ConstantTimeEquals(const std::string &a, const std::string &b) {
	if (a.size() != b.size()) {
		return false;
	}
	unsigned char difference = 0;
	for (size_t i = 0; i < a.size(); i++) {
		difference |= static_cast<unsigned char>(a[i] ^ b[i]);
	}
	return difference == 0;
}

//! Host must name the loopback interface and the bound port. Anything else is
//! a request that reached 127.0.0.1 under another name -- DNS rebinding.
bool HostAllowed(const GuardConfig &config, const std::string &host) {
	const auto lowered = ToLower(host);
	const auto port = std::to_string(config.port);
	return lowered == "127.0.0.1:" + port || lowered == "localhost:" + port;
}

bool IsOwnOrigin(const GuardConfig &config, const std::string &origin) {
	const auto port = std::to_string(config.port);
	return origin == "https://localhost:" + port || origin == "https://127.0.0.1:" + port;
}

//! A browser always sends Origin on a cross-origin request, which is every
//! request in the hosted profile. In the local profile the add-in is same-origin,
//! and a same-origin GET carries no Origin at all; the session token is what
//! guards it there, and an Origin that is present must still be our own.
bool OriginAllowed(const GuardConfig &config, const RequestHeaders &headers) {
	if (config.profile == Profile::HOSTED) {
		return headers.has_origin && !config.hosted_origin.empty() && headers.origin == config.hosted_origin;
	}
	return !headers.has_origin || IsOwnOrigin(config, headers.origin);
}

bool IsHostChar(char c) {
	return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.';
}

} // namespace

GuardVerdict CheckApiRequest(const GuardConfig &config, const RequestHeaders &headers) {
	if (!HostAllowed(config, headers.host)) {
		return GuardVerdict::BAD_HOST;
	}
	if (!OriginAllowed(config, headers)) {
		return GuardVerdict::BAD_ORIGIN;
	}
	if (config.profile == Profile::LOCAL &&
	    (!headers.has_session || config.session_token.empty() ||
	     !ConstantTimeEquals(headers.session, config.session_token))) {
		return GuardVerdict::BAD_SESSION;
	}
	return GuardVerdict::ALLOW;
}

GuardVerdict CheckPreflight(const GuardConfig &config, const RequestHeaders &headers) {
	if (!HostAllowed(config, headers.host)) {
		return GuardVerdict::BAD_HOST;
	}
	// A preflight is only ever sent cross-origin, so it must carry an Origin.
	if (!headers.has_origin || !OriginAllowed(config, headers)) {
		return GuardVerdict::BAD_ORIGIN;
	}
	return GuardVerdict::ALLOW;
}

std::string NormalizeHostedOrigin(const std::string &origin) {
	const auto lowered = ToLower(origin);
	const std::string scheme = "https://";
	if (lowered.compare(0, scheme.size(), scheme) != 0) {
		return "";
	}
	size_t at = scheme.size();
	const auto host_start = at;
	while (at < lowered.size() && IsHostChar(lowered[at])) {
		at++;
	}
	if (at == host_start || lowered[host_start] == '.' || lowered[at - 1] == '.') {
		return "";
	}
	if (at < lowered.size() && lowered[at] == ':') {
		const auto port_start = ++at;
		while (at < lowered.size() && std::isdigit(static_cast<unsigned char>(lowered[at]))) {
			at++;
		}
		const auto digits = at - port_start;
		if (digits == 0 || digits > 5 || std::stol(lowered.substr(port_start, digits)) > 65535) {
			return "";
		}
	}
	// Anything left over -- a path, a trailing slash, a query, user info -- is
	// not part of an origin, and the browser would never send it as one.
	if (at != lowered.size()) {
		return "";
	}
	return lowered;
}

const char *VerdictCode(GuardVerdict verdict) {
	switch (verdict) {
	case GuardVerdict::ALLOW:
		return "";
	case GuardVerdict::BAD_HOST:
		return "E_HOST";
	case GuardVerdict::BAD_ORIGIN:
		return "E_ORIGIN";
	case GuardVerdict::BAD_SESSION:
		return "E_SESSION";
	}
	return "E_INTERNAL";
}

} // namespace ofexcel
