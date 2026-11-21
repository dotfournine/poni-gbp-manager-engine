// Small crypto helpers on top of OpenSSL: hashing, random tokens, base64,
// and authenticated encryption for secrets stored in the database.
#pragma once

#include <cstddef>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

namespace poni::core {

// Lower-case hex SHA-256 of `data`.
std::string sha256_hex(std::string_view data);
// The 32 raw bytes of the SHA-256 of `data`.
std::string sha256_raw(std::string_view data);

// `n` bytes from OpenSSL's random generator. Throws if it fails.
std::string random_bytes(std::size_t n);
// A URL-safe random token carrying `bytes` bytes of entropy.
std::string random_token(std::size_t bytes = 32);

// Standard base64 with padding.
std::string base64_encode(std::string_view data);
// URL-safe base64 without padding (RFC 4648 section 5), as used by JWTs and PKCE.
std::string base64url_encode(std::string_view data);
// Accepts both alphabets, with or without padding. Nothing on invalid input.
std::optional<std::string> base64_decode(std::string_view text);

// Comparison whose duration doesn't depend on where the strings differ.
bool constant_time_equal(std::string_view a, std::string_view b);

// AES-256-GCM with a key from the environment (PONI_SECRET_KEY). Used for the
// `credentials` table: what is stored is useless without the key.
class SecretBox {
public:
    // `key` is base64 of exactly 32 bytes (`openssl rand -base64 32`).
    static std::expected<SecretBox, std::string> from_base64_key(std::string_view key);

    // Returns base64(nonce || ciphertext || tag) with a fresh random nonce.
    // `context` is authenticated but not stored (the row's name), so a value
    // can't be moved to another row.
    std::string seal(std::string_view plaintext, std::string_view context) const;

    // Fails if the key or context differ or the value was altered.
    std::expected<std::string, std::string> open(std::string_view sealed, std::string_view context) const;

    // Fingerprint of the key; safe to store and log.
    const std::string& key_id() const { return key_id_; }

private:
    explicit SecretBox(std::string key);
    std::string key_;
    std::string key_id_;
};

}  // namespace poni::core
