// The local CA and the server certificate (src/security/local_ca.cpp,
// TASKS.md A1). Pure: OpenSSL only, no DuckDB, no network. Every directory is a
// fresh one under the system temporary directory.

#include "ofexcel/local_ca.hpp"
#include "temporary_directory.hpp"

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#endif

using namespace ofexcel;
namespace fs = std::filesystem;

[[noreturn]] void CheckFailed(const char *expression, const char *file, int line) {
	std::cerr << file << ":" << line << ": check failed: " << expression << std::endl;
	std::abort();
}

#define CHECK(expression)                                                                                              \
	do {                                                                                                               \
		if (!(expression)) {                                                                                           \
			CheckFailed(#expression, __FILE__, __LINE__);                                                              \
		}                                                                                                              \
	} while (0)

namespace {

class TemporaryDirectory : public test_support::TemporaryDirectory {
public:
	TemporaryDirectory() : test_support::TemporaryDirectory("ofexcel_tls_test") {
	}
};

struct CertFree {
	void operator()(X509 *cert) const {
		X509_free(cert);
	}
};
struct KeyFree {
	void operator()(EVP_PKEY *key) const {
		EVP_PKEY_free(key);
	}
};
using Cert = std::unique_ptr<X509, CertFree>;
using Key = std::unique_ptr<EVP_PKEY, KeyFree>;

Cert LoadCert(const std::string &path) {
	const auto pem = ReadFileBytes(path);
	BIO *bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
	Cert cert(PEM_read_bio_X509(bio, nullptr, nullptr, nullptr));
	BIO_free(bio);
	CHECK(cert);
	return cert;
}

Key LoadKey(const std::string &path) {
	const auto pem = ReadFileBytes(path);
	BIO *bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
	Key key(PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr));
	BIO_free(bio);
	CHECK(key);
	return key;
}

std::string CommonName(X509 *cert) {
	char buffer[256] = {0};
	X509_NAME_get_text_by_NID(X509_get_subject_name(cert), NID_commonName, buffer, sizeof(buffer));
	return buffer;
}

bool IsCritical(X509 *cert, int nid) {
	const int index = X509_get_ext_by_NID(cert, nid, -1);
	return index >= 0 && X509_EXTENSION_get_critical(X509_get_ext(cert, index)) == 1;
}

//! Whole days from now until the certificate expires, rounded down.
long DaysLeft(X509 *cert) {
	int days = 0;
	int seconds = 0;
	CHECK(ASN1_TIME_diff(&days, &seconds, nullptr, X509_get0_notAfter(cert)) == 1);
	return days;
}

//! The general names in a GENERAL_NAMES stack, as "DNS:…" or "IP:<hex bytes>".
std::string Describe(const GENERAL_NAME *name) {
	if (name->type == GEN_DNS) {
		const auto *value = name->d.dNSName;
		return "DNS:" + std::string(reinterpret_cast<const char *>(ASN1_STRING_get0_data(value)),
		                            static_cast<size_t>(ASN1_STRING_length(value)));
	}
	if (name->type == GEN_IPADD) {
		const auto *value = name->d.iPAddress;
		std::string hex = "IP:";
		static const char *const HEX = "0123456789abcdef";
		const auto *bytes = ASN1_STRING_get0_data(value);
		for (int i = 0; i < ASN1_STRING_length(value); i++) {
			hex += HEX[bytes[i] >> 4];
			hex += HEX[bytes[i] & 0x0f];
		}
		return hex;
	}
	return "other:" + std::to_string(name->type);
}

std::vector<std::string> SubjectAltNames(X509 *cert) {
	std::vector<std::string> names;
	auto *sans = static_cast<GENERAL_NAMES *>(X509_get_ext_d2i(cert, NID_subject_alt_name, nullptr, nullptr));
	CHECK(sans);
	for (int i = 0; i < sk_GENERAL_NAME_num(sans); i++) {
		names.push_back(Describe(sk_GENERAL_NAME_value(sans, i)));
	}
	GENERAL_NAMES_free(sans);
	return names;
}

//! X509_V_OK, or why `cert` does not verify against `ca` as a TLS server for
//! `host` (a DNS name or an IP address).
int VerifyAsServer(X509 *ca, X509 *cert, const std::string &host) {
	X509_STORE *store = X509_STORE_new();
	X509_STORE_add_cert(store, ca);
	X509_STORE_CTX *context = X509_STORE_CTX_new();
	X509_STORE_CTX_init(context, store, cert, nullptr);
	X509_STORE_CTX_set_purpose(context, X509_PURPOSE_SSL_SERVER);
	auto *param = X509_STORE_CTX_get0_param(context);
	if (host.find_first_not_of("0123456789.") == std::string::npos) {
		X509_VERIFY_PARAM_set1_ip_asc(param, host.c_str());
	} else {
		X509_VERIFY_PARAM_set1_host(param, host.c_str(), 0);
	}
	const int verified = X509_verify_cert(context);
	const int error = verified == 1 ? X509_V_OK : X509_STORE_CTX_get_error(context);
	X509_STORE_CTX_free(context);
	X509_STORE_free(store);
	ERR_clear_error();
	return error;
}

//! A certificate for `san` signed with the CA's key, the way a stolen ca.key
//! would be used. Name constraints must make every client refuse it.
Cert ForgeWithCaKey(const TlsFiles &files, const char *san) {
	auto ca = LoadCert(files.ca_cert);
	auto ca_key = LoadKey(files.ca_key);
	Key key(EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "P-256"));
	Cert cert(X509_new());
	X509_set_version(cert.get(), X509_VERSION_3);
	ASN1_INTEGER_set(X509_get_serialNumber(cert.get()), 4242);
	X509_NAME_add_entry_by_txt(X509_get_subject_name(cert.get()), "CN", MBSTRING_UTF8,
	                           reinterpret_cast<const unsigned char *>("forged"), -1, -1, 0);
	X509_set_issuer_name(cert.get(), X509_get_subject_name(ca.get()));
	X509_gmtime_adj(X509_getm_notBefore(cert.get()), -300);
	X509_time_adj_ex(X509_getm_notAfter(cert.get()), 30, 0, nullptr);
	X509_set_pubkey(cert.get(), key.get());
	X509V3_CTX context;
	X509V3_set_ctx_nodb(&context);
	X509V3_set_ctx(&context, ca.get(), cert.get(), nullptr, nullptr, 0);
	for (const auto &extension : std::vector<std::pair<int, const char *>> {
	         {NID_basic_constraints, "critical,CA:FALSE"}, {NID_ext_key_usage, "serverAuth"}, {NID_subject_alt_name, san}}) {
		X509_EXTENSION *made = X509V3_EXT_nconf_nid(nullptr, &context, extension.first, extension.second);
		CHECK(made);
		X509_add_ext(cert.get(), made, -1);
		X509_EXTENSION_free(made);
	}
	CHECK(X509_sign(cert.get(), ca_key.get(), EVP_sha256()) > 0);
	return cert;
}

#ifndef _WIN32
int ModeOf(const std::string &path) {
	struct stat info;
	CHECK(::stat(path.c_str(), &info) == 0);
	return static_cast<int>(info.st_mode & 0777);
}
#endif

void TestInstallCreatesTheFiles() {
	TemporaryDirectory temporary;
	const auto dir = temporary.path + "/nested/tls";
	const auto result = InstallLocalCa(dir);
	CHECK(result.ca_created);
	CHECK(result.server_issued);
	const auto files = TlsFilesIn(dir);
	for (const auto &path : {files.ca_cert, files.ca_key, files.server_cert, files.server_key}) {
		CHECK(fs::exists(fs::u8path(path)));
		CHECK(!fs::exists(fs::u8path(path + ".tmp")));
	}
#ifndef _WIN32
	CHECK(ModeOf(files.ca_key) == 0600);
	CHECK(ModeOf(files.server_key) == 0600);
	CHECK(ModeOf(dir) == 0700);
#endif
}

void TestCaCertificate() {
	TemporaryDirectory temporary;
	InstallLocalCa(temporary.path);
	auto ca = LoadCert(TlsFilesIn(temporary.path).ca_cert);

	CHECK(CommonName(ca.get()) == CA_COMMON_NAME);
	CHECK(X509_check_ca(ca.get()) >= 1);
	CHECK(IsCritical(ca.get(), NID_basic_constraints));
	auto *constraints = static_cast<BASIC_CONSTRAINTS *>(X509_get_ext_d2i(ca.get(), NID_basic_constraints, nullptr, nullptr));
	CHECK(constraints && constraints->ca);
	CHECK(constraints->pathlen && ASN1_INTEGER_get(constraints->pathlen) == 0);
	BASIC_CONSTRAINTS_free(constraints);
	CHECK((X509_get_key_usage(ca.get()) & KU_KEY_CERT_SIGN) != 0);

	// Name constraints: permitted localhost and 127.0.0.1/32, nothing excluded.
	CHECK(IsCritical(ca.get(), NID_name_constraints));
	auto *names = static_cast<NAME_CONSTRAINTS *>(X509_get_ext_d2i(ca.get(), NID_name_constraints, nullptr, nullptr));
	CHECK(names && names->permittedSubtrees);
	CHECK(!names->excludedSubtrees || sk_GENERAL_SUBTREE_num(names->excludedSubtrees) == 0);
	std::vector<std::string> permitted;
	for (int i = 0; i < sk_GENERAL_SUBTREE_num(names->permittedSubtrees); i++) {
		permitted.push_back(Describe(sk_GENERAL_SUBTREE_value(names->permittedSubtrees, i)->base));
	}
	NAME_CONSTRAINTS_free(names);
	CHECK((permitted == std::vector<std::string> {"DNS:localhost", "IP:7f000001ffffffff"}));

	auto key = LoadKey(TlsFilesIn(temporary.path).ca_key);
	CHECK(EVP_PKEY_is_a(key.get(), "EC"));
	CHECK(EVP_PKEY_get_bits(key.get()) == 256);

	const auto days = DaysLeft(ca.get());
	CHECK(days >= CA_VALIDITY_DAYS - 1 && days <= CA_VALIDITY_DAYS);
}

void TestServerCertificate() {
	TemporaryDirectory temporary;
	InstallLocalCa(temporary.path);
	const auto files = TlsFilesIn(temporary.path);
	auto ca = LoadCert(files.ca_cert);
	auto cert = LoadCert(files.server_cert);

	CHECK((SubjectAltNames(cert.get()) == std::vector<std::string> {"DNS:localhost", "IP:7f000001"}));
	CHECK(X509_check_ca(cert.get()) == 0);
	CHECK((X509_get_extended_key_usage(cert.get()) & XKU_SSL_SERVER) != 0);
	CHECK(X509_check_private_key(cert.get(), LoadKey(files.server_key).get()) == 1);

	const auto days = DaysLeft(cert.get());
	CHECK(days >= SERVER_VALIDITY_DAYS - 1 && days <= SERVER_VALIDITY_DAYS);

	CHECK(VerifyAsServer(ca.get(), cert.get(), "localhost") == X509_V_OK);
	CHECK(VerifyAsServer(ca.get(), cert.get(), "127.0.0.1") == X509_V_OK);
	CHECK(VerifyAsServer(ca.get(), cert.get(), "example.com") == X509_V_ERR_HOSTNAME_MISMATCH);
}

void TestNameConstraintsRefuseOtherNames() {
	TemporaryDirectory temporary;
	InstallLocalCa(temporary.path);
	const auto files = TlsFilesIn(temporary.path);
	auto ca = LoadCert(files.ca_cert);

	auto forged_dns = ForgeWithCaKey(files, "DNS:example.com");
	CHECK(VerifyAsServer(ca.get(), forged_dns.get(), "example.com") == X509_V_ERR_PERMITTED_VIOLATION);
	auto forged_ip = ForgeWithCaKey(files, "IP:10.0.0.1");
	CHECK(VerifyAsServer(ca.get(), forged_ip.get(), "10.0.0.1") == X509_V_ERR_PERMITTED_VIOLATION);
	// The constraints admit what the daemon's own certificate names.
	auto loopback = ForgeWithCaKey(files, "DNS:localhost,IP:127.0.0.1");
	CHECK(VerifyAsServer(ca.get(), loopback.get(), "localhost") == X509_V_OK);
}

void TestReinstallKeepsTheCa() {
	TemporaryDirectory temporary;
	InstallLocalCa(temporary.path);
	const auto files = TlsFilesIn(temporary.path);
	const auto ca_before = ReadFileBytes(files.ca_cert);
	const auto server_before = ReadFileBytes(files.server_cert);

	const auto again = InstallLocalCa(temporary.path);
	CHECK(!again.ca_created);
	CHECK(!again.server_issued);
	CHECK(ReadFileBytes(files.ca_cert) == ca_before);
	CHECK(ReadFileBytes(files.server_cert) == server_before);
}

void TestExpiringCertificateIsRenewedFromTheSameCa() {
	TemporaryDirectory temporary;
	InstallLocalCa(temporary.path);
	const auto files = TlsFilesIn(temporary.path);
	const auto ca_before = ReadFileBytes(files.ca_cert);
	const auto ca_key_before = ReadFileBytes(files.ca_key);

	IssueServerCertificate(files, SERVER_RENEW_DAYS - 1);
	CHECK(DaysLeft(LoadCert(files.server_cert).get()) < SERVER_RENEW_DAYS);

	const auto pem = LoadServerCertificate(temporary.path);
	CHECK(pem.renewed);
	CHECK(pem.cert_pem == ReadFileBytes(files.server_cert));
	CHECK(pem.key_pem == ReadFileBytes(files.server_key));
	CHECK(ReadFileBytes(files.ca_cert) == ca_before);
	CHECK(ReadFileBytes(files.ca_key) == ca_key_before);
	auto renewed = LoadCert(files.server_cert);
	CHECK(DaysLeft(renewed.get()) >= SERVER_VALIDITY_DAYS - 1);
	CHECK(VerifyAsServer(LoadCert(files.ca_cert).get(), renewed.get(), "127.0.0.1") == X509_V_OK);

	// Still valid for long enough: left alone.
	IssueServerCertificate(files, SERVER_RENEW_DAYS + 2);
	CHECK(!LoadServerCertificate(temporary.path).renewed);
	CHECK(!LoadServerCertificate(temporary.path).renewed);
}

void TestBrokenServerCertificateIsReissued() {
	TemporaryDirectory temporary;
	InstallLocalCa(temporary.path);
	const auto files = TlsFilesIn(temporary.path);

	// A key that does not match the certificate, as a crash between the two
	// writes would leave.
	fs::copy_file(fs::u8path(files.ca_key), fs::u8path(files.server_key), fs::copy_options::overwrite_existing);
	CHECK(LoadServerCertificate(temporary.path).renewed);

	// A certificate from another CA.
	TemporaryDirectory other;
	InstallLocalCa(other.path);
	const auto other_files = TlsFilesIn(other.path);
	fs::copy_file(fs::u8path(other_files.server_cert), fs::u8path(files.server_cert), fs::copy_options::overwrite_existing);
	fs::copy_file(fs::u8path(other_files.server_key), fs::u8path(files.server_key), fs::copy_options::overwrite_existing);
	CHECK(LoadServerCertificate(temporary.path).renewed);
	CHECK(VerifyAsServer(LoadCert(files.ca_cert).get(), LoadCert(files.server_cert).get(), "localhost") == X509_V_OK);

	// No certificate at all.
	fs::remove(fs::u8path(files.server_cert));
	CHECK(LoadServerCertificate(temporary.path).renewed);
}

void TestNoCaIsMissing() {
	TemporaryDirectory temporary;
	bool thrown = false;
	try {
		LoadServerCertificate(temporary.path);
	} catch (const TlsError &error) {
		thrown = true;
		CHECK(error.kind == TlsError::Kind::MISSING);
		CHECK(std::string(error.what()).find("ofexceld install-cert") != std::string::npos);
	}
	CHECK(thrown);
	// Nothing created on the way.
	CHECK(!fs::exists(fs::u8path(TlsFilesIn(temporary.path).server_cert)));
}

void TestCaWithoutKeyIsInvalid() {
	TemporaryDirectory temporary;
	InstallLocalCa(temporary.path);
	fs::remove(fs::u8path(TlsFilesIn(temporary.path).ca_key));
	bool thrown = false;
	try {
		LoadServerCertificate(temporary.path);
	} catch (const TlsError &error) {
		thrown = true;
		CHECK(error.kind == TlsError::Kind::INVALID);
	}
	CHECK(thrown);
}

void TestCertificateIdentity() {
	TemporaryDirectory temporary;
	InstallLocalCa(temporary.path);
	const auto files = TlsFilesIn(temporary.path);
	const auto identity = ReadCertificateIdentity(files.ca_cert);
	CHECK(identity.sha1_hex.size() == 40);
	CHECK(identity.sha1_hex.find_first_not_of("0123456789ABCDEF") == std::string::npos);

	unsigned char digest[EVP_MAX_MD_SIZE];
	unsigned int size = 0;
	CHECK(X509_digest(LoadCert(files.ca_cert).get(), EVP_sha1(), digest, &size) == 1);
	CHECK(size == 20);
	CHECK(identity.der.size() > 100);
	CHECK(static_cast<unsigned char>(identity.der[0]) == 0x30); // DER SEQUENCE
	char first[3];
	std::snprintf(first, sizeof(first), "%02X", digest[0]);
	CHECK(identity.sha1_hex.substr(0, 2) == first);
}

} // namespace

int main() {
	TestInstallCreatesTheFiles();
	TestCaCertificate();
	TestServerCertificate();
	TestNameConstraintsRefuseOtherNames();
	TestReinstallKeepsTheCa();
	TestExpiringCertificateIsRenewedFromTheSameCa();
	TestBrokenServerCertificateIsReissued();
	TestNoCaIsMissing();
	TestCaWithoutKeyIsInvalid();
	TestCertificateIdentity();
	std::cout << "tls_test: all checks passed\n";
	return 0;
}
