// TLS client context for outbound HTTPS (Google's APIs, the model providers).
#pragma once

#include <boost/asio/ssl/context.hpp>

#include <string>

namespace poni::core {

struct TlsSetup {
    boost::asio::ssl::context ctx;
    std::string ca_source;  // where the trusted CA bundle came from, for the log
};

// Client context with peer verification and TLS >= 1.2.
//
// vcpkg's OpenSSL has its OPENSSLDIR inside the vcpkg tree, so its default
// verify paths hold no CA certificates. The bundle is therefore looked up in
// this order: $SSL_CERT_FILE, the OS bundle (macOS, Debian/Ubuntu, RHEL),
// then OpenSSL's defaults. Throws if $SSL_CERT_FILE is set but unreadable.
TlsSetup make_tls_client_context();

}  // namespace poni::core
