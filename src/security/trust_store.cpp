#include "ofexcel/trust_store.hpp"

#include "ofexcel/paths.hpp"

#include <stdexcept>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wincrypt.h>
#elif defined(__APPLE__)
#include <cstdlib>
#include <spawn.h>
#include <sys/wait.h>
extern char **environ;
#endif

namespace ofexcel {

#if defined(__APPLE__)

namespace {

const char *const SECURITY_TOOL = "/usr/bin/security";

//! Runs `security` with `arguments` directly, without a shell, and returns its
//! exit status. Its output goes to ours, so the user sees what it says.
int RunSecurity(const std::vector<std::string> &arguments) {
	std::vector<char *> argv;
	argv.push_back(const_cast<char *>(SECURITY_TOOL));
	for (const auto &argument : arguments) {
		argv.push_back(const_cast<char *>(argument.c_str()));
	}
	argv.push_back(nullptr);
	pid_t pid = 0;
	if (posix_spawn(&pid, SECURITY_TOOL, nullptr, nullptr, argv.data(), environ) != 0) {
		throw std::runtime_error(std::string("Не удалось запустить ") + SECURITY_TOOL);
	}
	int status = 0;
	while (waitpid(pid, &status, 0) < 0) {
	}
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

std::string LoginKeychain() {
	const char *home = std::getenv("HOME");
	if (!home || !*home) {
		throw std::runtime_error("Не задана переменная окружения HOME: не найти связку ключей входа");
	}
	return JoinPath(home, "Library/Keychains/login.keychain-db");
}

} // namespace

void TrustCa(const std::string &ca_cert_path, const CertificateIdentity &) {
	// User trust settings (no -d), so no administrator password: macOS asks for
	// the user's own password to change them.
	const int status = RunSecurity({"add-trusted-cert", "-r", "trustRoot", "-k", LoginKeychain(), ca_cert_path});
	if (status != 0) {
		throw std::runtime_error("macOS не добавил локальный CA в доверенные (security add-trusted-cert, код " +
		                         std::to_string(status) + "). Возможно, вход в систему не подтверждён паролем");
	}
}

void UntrustCa(const std::string &ca_cert_path, const CertificateIdentity &ca) {
	const int trust_status = RunSecurity({"remove-trusted-cert", ca_cert_path});
	const int delete_status = RunSecurity({"delete-certificate", "-Z", ca.sha1_hex, LoginKeychain()});
	if (trust_status != 0 && delete_status != 0) {
		throw std::runtime_error("macOS не удалил локальный CA из связки ключей входа (security, коды " +
		                         std::to_string(trust_status) + ", " + std::to_string(delete_status) +
		                         "). Если CA там нет, повторите с --no-trust");
	}
}

#elif defined(_WIN32)

namespace {

HCERTSTORE OpenUserRoot() {
	HCERTSTORE store = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0, CERT_SYSTEM_STORE_CURRENT_USER, L"ROOT");
	if (!store) {
		throw std::runtime_error("Не удалось открыть хранилище сертификатов CurrentUser\\ROOT (код " +
		                         std::to_string(GetLastError()) + ")");
	}
	return store;
}

std::vector<BYTE> HexToBytes(const std::string &hex) {
	std::vector<BYTE> bytes;
	for (size_t i = 0; i + 1 < hex.size(); i += 2) {
		bytes.push_back(static_cast<BYTE>(std::stoi(hex.substr(i, 2), nullptr, 16)));
	}
	return bytes;
}

} // namespace

void TrustCa(const std::string &, const CertificateIdentity &ca) {
	auto store = OpenUserRoot();
	// Windows shows its own confirmation dialog for CurrentUser\ROOT.
	const BOOL added = CertAddEncodedCertificateToStore(
	    store, X509_ASN_ENCODING, reinterpret_cast<const BYTE *>(ca.der.data()), static_cast<DWORD>(ca.der.size()),
	    CERT_STORE_ADD_REPLACE_EXISTING, nullptr);
	const DWORD error = added ? 0 : GetLastError();
	CertCloseStore(store, 0);
	if (!added) {
		throw std::runtime_error(error == ERROR_CANCELLED
		                             ? "Добавление локального CA в доверенные отменено пользователем"
		                             : "Windows не добавил локальный CA в CurrentUser\\ROOT (код " +
		                                   std::to_string(error) + ")");
	}
}

void UntrustCa(const std::string &, const CertificateIdentity &ca) {
	auto store = OpenUserRoot();
	auto hash = HexToBytes(ca.sha1_hex);
	CRYPT_HASH_BLOB blob {static_cast<DWORD>(hash.size()), hash.data()};
	PCCERT_CONTEXT found = CertFindCertificateInStore(store, X509_ASN_ENCODING, 0, CERT_FIND_SHA1_HASH, &blob, nullptr);
	// Already gone counts as removed.
	BOOL removed = TRUE;
	DWORD error = 0;
	if (found) {
		// Frees `found`, whatever the outcome.
		removed = CertDeleteCertificateFromStore(found);
		error = removed ? 0 : GetLastError();
	}
	CertCloseStore(store, 0);
	if (!removed) {
		throw std::runtime_error(error == ERROR_CANCELLED
		                             ? "Удаление локального CA из доверенных отменено пользователем"
		                             : "Windows не удалил локальный CA из CurrentUser\\ROOT (код " +
		                                   std::to_string(error) + ")");
	}
}

#else

void TrustCa(const std::string &, const CertificateIdentity &) {
	throw std::runtime_error("Установка CA в доверенные на этой платформе не поддерживается; используйте --no-trust");
}

void UntrustCa(const std::string &, const CertificateIdentity &) {
	throw std::runtime_error("Удаление CA из доверенных на этой платформе не поддерживается; используйте --no-trust");
}

#endif

} // namespace ofexcel
