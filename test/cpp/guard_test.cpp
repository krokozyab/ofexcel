// Vectors for the request guard (CONTRACTS.md §8.1). Pure: no DuckDB, no network.
// Each vector here is a row in CONTRACTS.md §8.1.1; change both together.

#include "ofexcel/request_guard.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

using namespace ofexcel;

namespace {

int failures = 0;

void Expect(const char *id, GuardVerdict actual, GuardVerdict expected) {
	if (actual != expected) {
		std::cerr << id << ": expected " << VerdictCode(expected) << ", got " << VerdictCode(actual) << "\n";
		failures++;
	}
}

void ExpectOrigin(const char *id, const std::string &input, const std::string &expected) {
	const auto actual = NormalizeHostedOrigin(input);
	if (actual != expected) {
		std::cerr << id << ": NormalizeHostedOrigin(\"" << input << "\") = \"" << actual << "\", expected \""
		          << expected << "\"\n";
		failures++;
	}
}

RequestHeaders Headers(const std::string &host, const char *origin = nullptr, const char *session = nullptr) {
	RequestHeaders headers;
	headers.host = host;
	if (origin) {
		headers.has_origin = true;
		headers.origin = origin;
	}
	if (session) {
		headers.has_session = true;
		headers.session = session;
	}
	return headers;
}

GuardConfig Hosted() {
	GuardConfig config;
	config.profile = Profile::HOSTED;
	config.port = 47321;
	config.hosted_origin = "https://app.example.com";
	return config;
}

GuardConfig Local() {
	GuardConfig config;
	config.profile = Profile::LOCAL;
	config.port = 47321;
	config.session_token = "s3cr3t";
	return config;
}

} // namespace

int main() {
	const auto hosted = Hosted();
	const char *origin = "https://app.example.com";

	Expect("G1", CheckApiRequest(hosted, Headers("127.0.0.1:47321", origin)), GuardVerdict::ALLOW);
	Expect("G2", CheckApiRequest(hosted, Headers("localhost:47321", origin)), GuardVerdict::ALLOW);
	Expect("G3", CheckApiRequest(hosted, Headers("LOCALHOST:47321", origin)), GuardVerdict::ALLOW);
	Expect("G4", CheckApiRequest(hosted, Headers("127.0.0.1:47322", origin)), GuardVerdict::BAD_HOST);
	Expect("G5", CheckApiRequest(hosted, Headers("evil.example.com", origin)), GuardVerdict::BAD_HOST);
	Expect("G6", CheckApiRequest(hosted, Headers("", origin)), GuardVerdict::BAD_HOST);
	Expect("G7", CheckApiRequest(hosted, Headers("127.0.0.1:47321", "https://evil.example.com")),
	       GuardVerdict::BAD_ORIGIN);
	Expect("G8", CheckApiRequest(hosted, Headers("127.0.0.1:47321")), GuardVerdict::BAD_ORIGIN);
	Expect("G9", CheckApiRequest(hosted, Headers("127.0.0.1:47321", "https://app.example.com/")),
	       GuardVerdict::BAD_ORIGIN);
	Expect("G10", CheckApiRequest(hosted, Headers("127.0.0.1:47321", "null")), GuardVerdict::BAD_ORIGIN);
	Expect("G11", CheckApiRequest(hosted, Headers("127.0.0.1:47321", "http://app.example.com")),
	       GuardVerdict::BAD_ORIGIN);

	const auto local = Local();
	Expect("G12", CheckApiRequest(local, Headers("localhost:47321", nullptr, "s3cr3t")), GuardVerdict::ALLOW);
	Expect("G13", CheckApiRequest(local, Headers("localhost:47321", "https://localhost:47321", "s3cr3t")),
	       GuardVerdict::ALLOW);
	Expect("G14", CheckApiRequest(local, Headers("localhost:47321", "https://app.example.com", "s3cr3t")),
	       GuardVerdict::BAD_ORIGIN);
	Expect("G15", CheckApiRequest(local, Headers("localhost:47321")), GuardVerdict::BAD_SESSION);
	Expect("G16", CheckApiRequest(local, Headers("localhost:47321", nullptr, "guess")), GuardVerdict::BAD_SESSION);

	Expect("G17", CheckPreflight(hosted, Headers("127.0.0.1:47321", origin)), GuardVerdict::ALLOW);
	Expect("G18", CheckPreflight(hosted, Headers("127.0.0.1:47321", "https://evil.example.com")),
	       GuardVerdict::BAD_ORIGIN);
	Expect("G19", CheckPreflight(hosted, Headers("127.0.0.1:47321")), GuardVerdict::BAD_ORIGIN);

	Expect("G20", CheckStaticRequest(local, Headers("localhost:47321")), GuardVerdict::ALLOW);
	Expect("G21", CheckStaticRequest(local, Headers("localhost:47321", "https://evil.example.com")),
	       GuardVerdict::ALLOW);
	Expect("G22", CheckStaticRequest(local, Headers("evil.example.com")), GuardVerdict::BAD_HOST);

	ExpectOrigin("O1", "https://app.example.com", "https://app.example.com");
	ExpectOrigin("O2", "https://APP.Example.com:8443", "https://app.example.com:8443");
	ExpectOrigin("O3", "http://app.example.com", "");
	ExpectOrigin("O4", "https://app.example.com/", "");
	ExpectOrigin("O5", "https://app.example.com/addin", "");
	ExpectOrigin("O6", "https://user@app.example.com", "");
	ExpectOrigin("O7", "https://app.example.com:99999", "");
	ExpectOrigin("O8", "https://", "");
	ExpectOrigin("O9", "", "");
	ExpectOrigin("O10", "https://.example.com", "");

	if (failures) {
		std::cerr << failures << " guard vector(s) failed\n";
		return EXIT_FAILURE;
	}
	std::cout << "guard_test: all vectors passed\n";
	return EXIT_SUCCESS;
}
