#pragma once

// The local CA and the server certificate the daemon serves HTTPS with
// (DECISIONS.md R6, CONTRACTS.md §8.1, SPEC.md §12.4).
//
// Pure: OpenSSL and the standard library only, no DuckDB, no httplib. The
// OpenSSL types stay out of this header, so a file that needs <wincrypt.h>
// (whose macros collide with OpenSSL's names) can still include it.

#include <stdexcept>
#include <string>

namespace ofexcel {

constexpr int CA_VALIDITY_DAYS = 3650;
constexpr int SERVER_VALIDITY_DAYS = 397;
//! A server certificate with fewer days left than this is reissued at start.
constexpr int SERVER_RENEW_DAYS = 30;

constexpr const char *CA_COMMON_NAME = "QExcel Local CA";

//! The four files in a TLS directory.
struct TlsFiles {
	std::string dir;
	std::string ca_cert;
	std::string ca_key;
	std::string server_cert;
	std::string server_key;
};

TlsFiles TlsFilesIn(const std::string &dir);

class TlsError : public std::runtime_error {
public:
	enum class Kind {
		MISSING, //!< no CA in the directory: install-cert has not been run
		INVALID, //!< a file is there but unreadable, or the CA cannot be used
		IO,      //!< a file or directory cannot be written
	};
	TlsError(Kind kind, const std::string &message) : std::runtime_error(message), kind(kind) {
	}
	Kind kind;
};

struct InstallResult {
	bool ca_created = false;
	bool server_issued = false;
};

//! What `ofexceld install-cert` does on disk. Creates the directory (0700) and
//! the CA if there is none; keeps an existing CA, so reinstalling does not
//! orphan a CA the user already trusts. Issues the server certificate if it is
//! missing, expires within SERVER_RENEW_DAYS, or does not belong to this CA.
//! Keys are written 0600. Throws TlsError.
InstallResult InstallLocalCa(const std::string &dir);

//! The server certificate and key, as PEM, for the HTTPS server.
struct ServerPem {
	std::string cert_pem;
	std::string key_pem;
	//! True if the certificate was reissued on this call.
	bool renewed = false;
};

//! At server start: reads the server certificate, reissuing it from the same
//! CA if it is missing, expires within SERVER_RENEW_DAYS, or does not match its
//! key or the CA. Throws TlsError(MISSING) if the directory has no CA.
ServerPem LoadServerCertificate(const std::string &dir);

//! Issues a server certificate valid for `validity_days` from the CA in
//! `files`, replacing server.crt and server.key. InstallLocalCa and
//! LoadServerCertificate call it with SERVER_VALIDITY_DAYS; tests call it with
//! less to exercise renewal.
void IssueServerCertificate(const TlsFiles &files, int validity_days);

//! What the platform trust stores need to know about a CA certificate.
struct CertificateIdentity {
	std::string der;      //!< DER bytes
	std::string sha1_hex; //!< SHA-1 of the DER, upper-case hex
};

//! Reads a PEM certificate file. Throws TlsError.
CertificateIdentity ReadCertificateIdentity(const std::string &cert_path);

//! The local profile's session token: 32 bytes from RAND_bytes, 64 lower-case
//! hex digits. Throws TlsError(INVALID) if the generator fails.
std::string GenerateSessionToken();

//! The whole file; throws TlsError(MISSING) if it does not exist.
std::string ReadFileBytes(const std::string &path);

} // namespace ofexcel
