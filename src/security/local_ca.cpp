#include "ofexcel/local_ca.hpp"

#include "ofexcel/paths.hpp"

#include <openssl/bn.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace ofexcel {

namespace {

namespace fs = std::filesystem;

struct BioFree {
	void operator()(BIO *bio) const {
		BIO_free_all(bio);
	}
};
struct KeyFree {
	void operator()(EVP_PKEY *key) const {
		EVP_PKEY_free(key);
	}
};
struct CertFree {
	void operator()(X509 *cert) const {
		X509_free(cert);
	}
};
struct BignumFree {
	void operator()(BIGNUM *number) const {
		BN_free(number);
	}
};
struct ExtensionFree {
	void operator()(X509_EXTENSION *extension) const {
		X509_EXTENSION_free(extension);
	}
};

using Bio = std::unique_ptr<BIO, BioFree>;
using Key = std::unique_ptr<EVP_PKEY, KeyFree>;
using Cert = std::unique_ptr<X509, CertFree>;

fs::path FsPath(const std::string &utf8) {
	return fs::u8path(utf8);
}

bool Exists(const std::string &path) {
	std::error_code error;
	return fs::exists(FsPath(path), error);
}

//! The OpenSSL error queue as one line, emptied.
std::string OpenSslErrors() {
	std::string joined;
	while (const auto code = ERR_get_error()) {
		char buffer[256];
		ERR_error_string_n(code, buffer, sizeof(buffer));
		joined += joined.empty() ? "" : "; ";
		joined += buffer;
	}
	return joined;
}

[[noreturn]] void FailOpenSsl(const std::string &what) {
	const auto details = OpenSslErrors();
	throw TlsError(TlsError::Kind::INVALID, what + (details.empty() ? "" : " (" + details + ")"));
}

//! Writes through a temporary file and a rename, so a crash never leaves half a
//! key behind. `mode` applies on POSIX; on Windows the file inherits the ACL of
//! %LOCALAPPDATA%, which is already private to the user.
void WriteFileAtomically(const std::string &path, const std::string &content, int mode) {
	const auto temporary = path + ".tmp";
#ifdef _WIN32
	(void)mode;
	{
		std::ofstream file(FsPath(temporary), std::ios::binary | std::ios::trunc);
		file.write(content.data(), static_cast<std::streamsize>(content.size()));
		if (!file) {
			throw TlsError(TlsError::Kind::IO, "Не удалось записать " + temporary);
		}
	}
#else
	const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC, mode);
	if (fd < 0) {
		throw TlsError(TlsError::Kind::IO, "Не удалось создать " + temporary);
	}
	// open() honours the umask, and an existing file keeps its old mode.
	bool written = ::fchmod(fd, static_cast<mode_t>(mode)) == 0;
	size_t offset = 0;
	while (written && offset < content.size()) {
		const auto count = ::write(fd, content.data() + offset, content.size() - offset);
		written = count > 0;
		offset += written ? static_cast<size_t>(count) : 0;
	}
	written = ::close(fd) == 0 && written;
	if (!written) {
		::unlink(temporary.c_str());
		throw TlsError(TlsError::Kind::IO, "Не удалось записать " + temporary);
	}
#endif
	std::error_code error;
	fs::rename(FsPath(temporary), FsPath(path), error);
	if (error) {
		fs::remove(FsPath(temporary), error);
		throw TlsError(TlsError::Kind::IO, "Не удалось заменить " + path);
	}
}

void EnsureDirectory(const std::string &dir) {
	std::error_code error;
	const bool created = fs::create_directories(FsPath(dir), error);
	if (error) {
		throw TlsError(TlsError::Kind::IO, "Не удалось создать каталог " + dir + ": " + error.message());
	}
#ifndef _WIN32
	// Only a directory made here: --dir may name one the user owns for other things.
	if (created) {
		::chmod(dir.c_str(), 0700);
	}
#else
	(void)created;
#endif
}

Key GenerateKey() {
	Key key(EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "P-256"));
	if (!key) {
		FailOpenSsl("Не удалось создать ключ ECDSA P-256");
	}
	return key;
}

std::string KeyToPem(EVP_PKEY *key) {
	Bio bio(BIO_new(BIO_s_mem()));
	if (!bio || PEM_write_bio_PrivateKey(bio.get(), key, nullptr, nullptr, 0, nullptr, nullptr) != 1) {
		FailOpenSsl("Не удалось записать ключ в PEM");
	}
	char *data = nullptr;
	const auto size = BIO_get_mem_data(bio.get(), &data);
	return std::string(data, static_cast<size_t>(size));
}

std::string CertToPem(X509 *cert) {
	Bio bio(BIO_new(BIO_s_mem()));
	if (!bio || PEM_write_bio_X509(bio.get(), cert) != 1) {
		FailOpenSsl("Не удалось записать сертификат в PEM");
	}
	char *data = nullptr;
	const auto size = BIO_get_mem_data(bio.get(), &data);
	return std::string(data, static_cast<size_t>(size));
}

Cert ReadCert(const std::string &path) {
	const auto pem = ReadFileBytes(path);
	Bio bio(BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())));
	Cert cert(bio ? PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr) : nullptr);
	if (!cert) {
		FailOpenSsl("Файл " + path + " не является сертификатом PEM");
	}
	return cert;
}

Key ReadKey(const std::string &path) {
	const auto pem = ReadFileBytes(path);
	Bio bio(BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())));
	Key key(bio ? PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr) : nullptr);
	if (!key) {
		FailOpenSsl("Файл " + path + " не является ключом PEM");
	}
	return key;
}

//! 128 random bits, top bit clear so the DER integer stays positive.
void SetRandomSerial(X509 *cert) {
	unsigned char bytes[16];
	if (RAND_bytes(bytes, sizeof(bytes)) != 1) {
		FailOpenSsl("Не удалось получить случайный серийный номер");
	}
	bytes[0] &= 0x7f;
	std::unique_ptr<BIGNUM, BignumFree> number(BN_bin2bn(bytes, sizeof(bytes), nullptr));
	if (!number || !BN_to_ASN1_INTEGER(number.get(), X509_get_serialNumber(cert))) {
		FailOpenSsl("Не удалось задать серийный номер");
	}
}

void SetCommonName(X509_NAME *name, const char *common_name) {
	if (X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_UTF8, reinterpret_cast<const unsigned char *>(common_name), -1,
	                               -1, 0) != 1) {
		FailOpenSsl("Не удалось задать имя сертификата");
	}
}

//! notBefore five minutes back, so a clock a little behind does not reject a
//! certificate issued a moment ago.
void SetValidity(X509 *cert, int days) {
	if (!X509_gmtime_adj(X509_getm_notBefore(cert), -300) ||
	    !X509_time_adj_ex(X509_getm_notAfter(cert), days, 0, nullptr)) {
		FailOpenSsl("Не удалось задать срок действия");
	}
}

void AddExtension(X509 *cert, X509 *issuer, int nid, const char *value) {
	X509V3_CTX context;
	X509V3_set_ctx_nodb(&context);
	X509V3_set_ctx(&context, issuer, cert, nullptr, nullptr, 0);
	std::unique_ptr<X509_EXTENSION, ExtensionFree> extension(X509V3_EXT_nconf_nid(nullptr, &context, nid, value));
	if (!extension || X509_add_ext(cert, extension.get(), -1) != 1) {
		FailOpenSsl(std::string("Не удалось добавить расширение ") + OBJ_nid2sn(nid));
	}
}

void Sign(X509 *cert, EVP_PKEY *key) {
	if (X509_sign(cert, key, EVP_sha256()) <= 0) {
		FailOpenSsl("Не удалось подписать сертификат");
	}
}

void CreateCa(const TlsFiles &files) {
	auto key = GenerateKey();
	Cert cert(X509_new());
	if (!cert || X509_set_version(cert.get(), X509_VERSION_3) != 1) {
		FailOpenSsl("Не удалось создать сертификат CA");
	}
	SetRandomSerial(cert.get());
	SetCommonName(X509_get_subject_name(cert.get()), CA_COMMON_NAME);
	X509_set_issuer_name(cert.get(), X509_get_subject_name(cert.get()));
	SetValidity(cert.get(), CA_VALIDITY_DAYS);
	X509_set_pubkey(cert.get(), key.get());

	AddExtension(cert.get(), cert.get(), NID_basic_constraints, "critical,CA:TRUE,pathlen:0");
	AddExtension(cert.get(), cert.get(), NID_key_usage, "critical,keyCertSign,cRLSign");
	AddExtension(cert.get(), cert.get(), NID_subject_key_identifier, "hash");
	// The CA can vouch for loopback names only: a stolen ca.key cannot mint a
	// certificate any client would accept for another site.
	AddExtension(cert.get(), cert.get(), NID_name_constraints,
	             "critical,permitted;DNS:localhost,permitted;IP:127.0.0.1/255.255.255.255");
	Sign(cert.get(), key.get());

	// The key first: a certificate without its key is worthless, while a key
	// without a certificate is simply replaced on the next run.
	WriteFileAtomically(files.ca_key, KeyToPem(key.get()), 0600);
	WriteFileAtomically(files.ca_cert, CertToPem(cert.get()), 0644);
}

struct Ca {
	Cert cert;
	Key key;
};

Ca LoadCa(const TlsFiles &files) {
	if (!Exists(files.ca_cert)) {
		throw TlsError(TlsError::Kind::MISSING, "В каталоге " + files.dir +
		                                            " нет локального CA QExcel: выполните ofexceld install-cert");
	}
	const auto broken = [&](const std::string &what) {
		return TlsError(TlsError::Kind::INVALID, what + ". Выполните ofexceld uninstall-cert, затем ofexceld install-cert");
	};
	if (!Exists(files.ca_key)) {
		throw broken("Нет ключа локального CA " + files.ca_key);
	}
	Ca ca {ReadCert(files.ca_cert), ReadKey(files.ca_key)};
	if (X509_check_private_key(ca.cert.get(), ca.key.get()) != 1) {
		ERR_clear_error();
		throw broken("Ключ " + files.ca_key + " не подходит к сертификату " + files.ca_cert);
	}
	if (X509_check_ca(ca.cert.get()) < 1) {
		throw broken("Сертификат " + files.ca_cert + " не является CA");
	}
	if (X509_cmp_current_time(X509_get0_notAfter(ca.cert.get())) <= 0) {
		throw broken("Срок действия локального CA истёк");
	}
	return ca;
}

//! Why the server certificate in `files` cannot be served as it is; "" if it can.
std::string ServerCertificateProblem(const TlsFiles &files, const Ca &ca) {
	if (!Exists(files.server_cert) || !Exists(files.server_key)) {
		return "missing";
	}
	try {
		auto cert = ReadCert(files.server_cert);
		auto key = ReadKey(files.server_key);
		if (X509_check_private_key(cert.get(), key.get()) != 1) {
			return "key does not match";
		}
		if (X509_check_issued(ca.cert.get(), cert.get()) != X509_V_OK ||
		    X509_verify(cert.get(), X509_get0_pubkey(ca.cert.get())) != 1) {
			return "not issued by this CA";
		}
		time_t renew_by = std::time(nullptr) + static_cast<time_t>(SERVER_RENEW_DAYS) * 24 * 60 * 60;
		// -1: notAfter is before renew_by; 0: notAfter could not be compared.
		if (X509_cmp_time(X509_get0_notAfter(cert.get()), &renew_by) <= 0) {
			return "expires soon";
		}
	} catch (const TlsError &) {
		return "unreadable";
	}
	ERR_clear_error();
	return "";
}

void IssueFromCa(const TlsFiles &files, const Ca &ca, int validity_days) {
	auto key = GenerateKey();
	Cert cert(X509_new());
	if (!cert || X509_set_version(cert.get(), X509_VERSION_3) != 1) {
		FailOpenSsl("Не удалось создать сертификат сервера");
	}
	SetRandomSerial(cert.get());
	SetCommonName(X509_get_subject_name(cert.get()), "localhost");
	X509_set_issuer_name(cert.get(), X509_get_subject_name(ca.cert.get()));
	SetValidity(cert.get(), validity_days);
	// A certificate outliving its CA would fail verification for its last days.
	if (ASN1_TIME_compare(X509_get0_notAfter(cert.get()), X509_get0_notAfter(ca.cert.get())) > 0) {
		X509_set1_notAfter(cert.get(), X509_get0_notAfter(ca.cert.get()));
	}
	X509_set_pubkey(cert.get(), key.get());

	AddExtension(cert.get(), ca.cert.get(), NID_basic_constraints, "critical,CA:FALSE");
	AddExtension(cert.get(), ca.cert.get(), NID_key_usage, "critical,digitalSignature");
	AddExtension(cert.get(), ca.cert.get(), NID_ext_key_usage, "serverAuth");
	AddExtension(cert.get(), ca.cert.get(), NID_subject_alt_name, "DNS:localhost,IP:127.0.0.1");
	AddExtension(cert.get(), ca.cert.get(), NID_subject_key_identifier, "hash");
	AddExtension(cert.get(), ca.cert.get(), NID_authority_key_identifier, "keyid:always");
	Sign(cert.get(), ca.key.get());

	// A crash between the two writes leaves a key that does not match the
	// certificate; ServerCertificateProblem notices and the next start reissues.
	WriteFileAtomically(files.server_key, KeyToPem(key.get()), 0600);
	WriteFileAtomically(files.server_cert, CertToPem(cert.get()), 0644);
}

} // namespace

TlsFiles TlsFilesIn(const std::string &dir) {
	TlsFiles files;
	files.dir = dir;
	files.ca_cert = JoinPath(dir, "ca.crt");
	files.ca_key = JoinPath(dir, "ca.key");
	files.server_cert = JoinPath(dir, "server.crt");
	files.server_key = JoinPath(dir, "server.key");
	return files;
}

std::string ReadFileBytes(const std::string &path) {
	std::ifstream file(FsPath(path), std::ios::binary);
	if (!file) {
		throw TlsError(TlsError::Kind::MISSING, "Нет файла " + path);
	}
	std::ostringstream content;
	content << file.rdbuf();
	return content.str();
}

InstallResult InstallLocalCa(const std::string &dir) {
	const auto files = TlsFilesIn(dir);
	EnsureDirectory(dir);
	InstallResult result;
	if (!Exists(files.ca_cert)) {
		CreateCa(files);
		result.ca_created = true;
	}
	const auto ca = LoadCa(files);
	if (!ServerCertificateProblem(files, ca).empty()) {
		IssueFromCa(files, ca, SERVER_VALIDITY_DAYS);
		result.server_issued = true;
	}
	return result;
}

ServerPem LoadServerCertificate(const std::string &dir) {
	const auto files = TlsFilesIn(dir);
	const auto ca = LoadCa(files);
	ServerPem pem;
	if (!ServerCertificateProblem(files, ca).empty()) {
		IssueFromCa(files, ca, SERVER_VALIDITY_DAYS);
		pem.renewed = true;
	}
	pem.cert_pem = ReadFileBytes(files.server_cert);
	pem.key_pem = ReadFileBytes(files.server_key);
	return pem;
}

void IssueServerCertificate(const TlsFiles &files, int validity_days) {
	IssueFromCa(files, LoadCa(files), validity_days);
}

CertificateIdentity ReadCertificateIdentity(const std::string &cert_path) {
	auto cert = ReadCert(cert_path);
	unsigned char *der = nullptr;
	const int size = i2d_X509(cert.get(), &der);
	if (size <= 0) {
		FailOpenSsl("Не удалось закодировать сертификат в DER");
	}
	CertificateIdentity identity;
	identity.der.assign(reinterpret_cast<const char *>(der), static_cast<size_t>(size));
	OPENSSL_free(der);

	unsigned char digest[EVP_MAX_MD_SIZE];
	unsigned int digest_size = 0;
	if (EVP_Digest(identity.der.data(), identity.der.size(), digest, &digest_size, EVP_sha1(), nullptr) != 1) {
		FailOpenSsl("Не удалось вычислить отпечаток сертификата");
	}
	static const char *const HEX = "0123456789ABCDEF";
	for (unsigned int i = 0; i < digest_size; i++) {
		identity.sha1_hex += HEX[digest[i] >> 4];
		identity.sha1_hex += HEX[digest[i] & 0x0f];
	}
	return identity;
}

} // namespace ofexcel
