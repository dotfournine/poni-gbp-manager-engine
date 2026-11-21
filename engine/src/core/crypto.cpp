#include "core/crypto.hpp"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <array>
#include <memory>
#include <stdexcept>

namespace poni::core {

namespace {

constexpr std::size_t kKeyBytes = 32;
constexpr std::size_t kNonceBytes = 12;
constexpr std::size_t kTagBytes = 16;

constexpr std::string_view kStandardAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
constexpr std::string_view kUrlAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

std::string encode(std::string_view data, std::string_view alphabet, bool pad) {
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 2 < data.size(); i += 3) {
        const unsigned n = (static_cast<unsigned char>(data[i]) << 16) | (static_cast<unsigned char>(data[i + 1]) << 8) |
                           static_cast<unsigned char>(data[i + 2]);
        out.push_back(alphabet[(n >> 18) & 63]);
        out.push_back(alphabet[(n >> 12) & 63]);
        out.push_back(alphabet[(n >> 6) & 63]);
        out.push_back(alphabet[n & 63]);
    }
    if (const auto rest = data.size() - i; rest == 1) {
        const unsigned n = static_cast<unsigned char>(data[i]) << 16;
        out.push_back(alphabet[(n >> 18) & 63]);
        out.push_back(alphabet[(n >> 12) & 63]);
        if (pad) out += "==";
    } else if (rest == 2) {
        const unsigned n = (static_cast<unsigned char>(data[i]) << 16) | (static_cast<unsigned char>(data[i + 1]) << 8);
        out.push_back(alphabet[(n >> 18) & 63]);
        out.push_back(alphabet[(n >> 12) & 63]);
        out.push_back(alphabet[(n >> 6) & 63]);
        if (pad) out.push_back('=');
    }
    return out;
}

struct CipherCtxDeleter {
    void operator()(EVP_CIPHER_CTX* ctx) const { EVP_CIPHER_CTX_free(ctx); }
};
using CipherCtx = std::unique_ptr<EVP_CIPHER_CTX, CipherCtxDeleter>;

const unsigned char* bytes(std::string_view s) { return reinterpret_cast<const unsigned char*>(s.data()); }
unsigned char* bytes(std::string& s) { return reinterpret_cast<unsigned char*>(s.data()); }

}  // namespace

std::string sha256_raw(std::string_view data) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int len = 0;
    if (EVP_Digest(data.data(), data.size(), digest.data(), &len, EVP_sha256(), nullptr) != 1) {
        throw std::runtime_error("SHA-256 failed");
    }
    return std::string(reinterpret_cast<const char*>(digest.data()), len);
}

std::string sha256_hex(std::string_view data) {
    static constexpr char kHex[] = "0123456789abcdef";
    const auto digest = sha256_raw(data);
    std::string out;
    out.reserve(digest.size() * 2);
    for (const char c : digest) {
        const auto b = static_cast<unsigned char>(c);
        out.push_back(kHex[b >> 4]);
        out.push_back(kHex[b & 0x0f]);
    }
    return out;
}

std::string random_bytes(std::size_t n) {
    std::string out(n, '\0');
    if (n > 0 && RAND_bytes(bytes(out), static_cast<int>(n)) != 1) throw std::runtime_error("random generator failed");
    return out;
}

std::string random_token(std::size_t n) { return base64url_encode(random_bytes(n)); }

std::string base64_encode(std::string_view data) { return encode(data, kStandardAlphabet, true); }
std::string base64url_encode(std::string_view data) { return encode(data, kUrlAlphabet, false); }

std::optional<std::string> base64_decode(std::string_view text) {
    const auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+' || c == '-') return 62;
        if (c == '/' || c == '_') return 63;
        return -1;
    };
    while (!text.empty() && text.back() == '=') text.remove_suffix(1);
    if (text.size() % 4 == 1) return std::nullopt;
    std::string out;
    out.reserve(text.size() * 3 / 4);
    unsigned buffer = 0;
    int bits = 0;
    for (const char c : text) {
        const int v = value(c);
        if (v < 0) return std::nullopt;
        buffer = (buffer << 6) | static_cast<unsigned>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((buffer >> bits) & 0xff));
        }
    }
    return out;
}

bool constant_time_equal(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    return CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}

SecretBox::SecretBox(std::string key)
    : key_(std::move(key)), key_id_(sha256_hex("poni-secret-key-id:" + key_).substr(0, 12)) {}

std::expected<SecretBox, std::string> SecretBox::from_base64_key(std::string_view key) {
    auto raw = base64_decode(key);
    if (!raw || raw->size() != kKeyBytes) {
        return std::unexpected("the secret key must be base64 of 32 bytes (openssl rand -base64 32)");
    }
    return SecretBox(std::move(*raw));
}

std::string SecretBox::seal(std::string_view plaintext, std::string_view context) const {
    const auto nonce = random_bytes(kNonceBytes);
    std::string ciphertext(plaintext.size(), '\0');
    std::string tag(kTagBytes, '\0');
    int len = 0;

    const CipherCtx ctx(EVP_CIPHER_CTX_new());
    const bool ok =
        ctx && EVP_EncryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, bytes(key_), bytes(nonce)) == 1 &&
        EVP_EncryptUpdate(ctx.get(), nullptr, &len, bytes(context), static_cast<int>(context.size())) == 1 &&
        EVP_EncryptUpdate(ctx.get(), bytes(ciphertext), &len, bytes(plaintext), static_cast<int>(plaintext.size())) == 1 &&
        EVP_EncryptFinal_ex(ctx.get(), bytes(ciphertext) + len, &len) == 1 &&
        EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_GET_TAG, static_cast<int>(kTagBytes), tag.data()) == 1;
    if (!ok) throw std::runtime_error("encryption failed");
    return base64_encode(nonce + ciphertext + tag);
}

std::expected<std::string, std::string> SecretBox::open(std::string_view sealed, std::string_view context) const {
    const auto raw = base64_decode(sealed);
    if (!raw || raw->size() < kNonceBytes + kTagBytes) return std::unexpected("the stored value is not valid");
    const std::string_view all(*raw);
    const auto nonce = all.substr(0, kNonceBytes);
    const auto ciphertext = all.substr(kNonceBytes, all.size() - kNonceBytes - kTagBytes);
    std::string tag(all.substr(all.size() - kTagBytes));
    std::string plaintext(ciphertext.size(), '\0');
    int len = 0;

    const CipherCtx ctx(EVP_CIPHER_CTX_new());
    const bool ok =
        ctx && EVP_DecryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, bytes(key_), bytes(nonce)) == 1 &&
        EVP_DecryptUpdate(ctx.get(), nullptr, &len, bytes(context), static_cast<int>(context.size())) == 1 &&
        EVP_DecryptUpdate(ctx.get(), bytes(plaintext), &len, bytes(ciphertext), static_cast<int>(ciphertext.size())) == 1 &&
        EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_TAG, static_cast<int>(kTagBytes), tag.data()) == 1 &&
        EVP_DecryptFinal_ex(ctx.get(), bytes(plaintext) + len, &len) == 1;
    if (!ok) return std::unexpected("the stored value can't be decrypted: wrong key, or the value was changed");
    return plaintext;
}

}  // namespace poni::core
