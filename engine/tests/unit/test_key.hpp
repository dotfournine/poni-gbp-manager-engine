// An ECDSA P-256 key made once per test binary, as an APNs auth key is
// (D-097): its .p8 text, and a check that a signature is the key's.
#pragma once

#include "core/crypto.hpp"

#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/x509.h>

#include <string>
#include <string_view>

namespace poni::test {

struct TestKey {
    EVP_PKEY* key = EVP_EC_gen("P-256");
    // As a .p8 file holds it between its BEGIN and END lines.
    std::string p8 = [this] {
        PKCS8_PRIV_KEY_INFO* info = EVP_PKEY2PKCS8(key);
        unsigned char* der = nullptr;
        const int n = i2d_PKCS8_PRIV_KEY_INFO(info, &der);
        std::string out = core::base64_encode(std::string_view(reinterpret_cast<const char*>(der), static_cast<std::size_t>(n)));
        OPENSSL_free(der);
        PKCS8_PRIV_KEY_INFO_free(info);
        return out;
    }();

    // Whether `raw` (r and s, 32 bytes each) is this key's signature of `data`.
    bool verifies(std::string_view data, std::string_view raw) const {
        if (raw.size() != 64) return false;
        ECDSA_SIG* sig = ECDSA_SIG_new();
        ECDSA_SIG_set0(sig, BN_bin2bn(reinterpret_cast<const unsigned char*>(raw.data()), 32, nullptr),
                       BN_bin2bn(reinterpret_cast<const unsigned char*>(raw.data()) + 32, 32, nullptr));
        unsigned char* der = nullptr;
        const int n = i2d_ECDSA_SIG(sig, &der);
        ECDSA_SIG_free(sig);
        EVP_MD_CTX* ctx = EVP_MD_CTX_new();
        const bool ok = EVP_DigestVerifyInit(ctx, nullptr, EVP_sha256(), nullptr, key) == 1 &&
                        EVP_DigestVerify(ctx, der, static_cast<std::size_t>(n), reinterpret_cast<const unsigned char*>(data.data()),
                                         data.size()) == 1;
        EVP_MD_CTX_free(ctx);
        OPENSSL_free(der);
        return ok;
    }
};

inline const TestKey& test_key() {
    static const TestKey key;
    return key;
}

}  // namespace poni::test
