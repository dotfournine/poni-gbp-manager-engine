#include "core/tls.hpp"

#include <gtest/gtest.h>

#include <cstdlib>

// vcpkg's OpenSSL ships no CA certificates, so the context must find the
// operating system's bundle.
TEST(Tls, LoadsTheOperatingSystemCaBundle) {
    ::unsetenv("SSL_CERT_FILE");
    const auto tls = poni::core::make_tls_client_context();
    EXPECT_TRUE(tls.ca_source.starts_with("/etc/")) << tls.ca_source;
}

TEST(Tls, SslCertFileOverrides) {
    ::setenv("SSL_CERT_FILE", "/nonexistent/bundle.pem", 1);
    EXPECT_ANY_THROW(poni::core::make_tls_client_context());
    ::unsetenv("SSL_CERT_FILE");
}
