#pragma once

// Puts the local CA into the current user's trusted roots and takes it out
// again (SPEC.md §12.4). No administrator rights: the macOS login keychain,
// the Windows CurrentUser\ROOT store. Both ask the user to confirm.
//
// No OpenSSL here: on Windows this file needs <wincrypt.h>, whose macros
// collide with OpenSSL's type names.

#include "ofexcel/local_ca.hpp"

#include <string>

namespace ofexcel {

//! Throws std::runtime_error, with a message for the user, if the platform
//! refuses or the user declines.
void TrustCa(const std::string &ca_cert_path, const CertificateIdentity &ca);

//! Removes the trust setting and the certificate. Throws std::runtime_error.
void UntrustCa(const std::string &ca_cert_path, const CertificateIdentity &ca);

} // namespace ofexcel
