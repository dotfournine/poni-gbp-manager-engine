#include "core/tls.hpp"

#include <array>
#include <cstdlib>
#include <filesystem>
#include <system_error>

namespace poni::core {

namespace ssl = boost::asio::ssl;

TlsSetup make_tls_client_context() {
    TlsSetup t{ssl::context{ssl::context::tls_client}, {}};
    t.ctx.set_verify_mode(ssl::verify_peer);
    t.ctx.set_options(ssl::context::default_workarounds | ssl::context::no_tlsv1 | ssl::context::no_tlsv1_1);

    if (const char* env = std::getenv("SSL_CERT_FILE"); env && *env) {
        t.ctx.load_verify_file(env);
        t.ca_source = std::string("$SSL_CERT_FILE=") + env;
        return t;
    }
    constexpr std::array bundles{
        "/etc/ssl/cert.pem",                   // macOS, Alpine
        "/etc/ssl/certs/ca-certificates.crt",  // Debian, Ubuntu
        "/etc/pki/tls/certs/ca-bundle.crt",    // RHEL, Fedora
    };
    for (const char* path : bundles) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(path, ec)) {
            t.ctx.load_verify_file(path);
            t.ca_source = path;
            return t;
        }
    }
    t.ctx.set_default_verify_paths();
    t.ca_source = "OpenSSL default verify paths";
    return t;
}

}  // namespace poni::core
