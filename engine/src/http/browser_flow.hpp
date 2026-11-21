// What dashboard sign-in (http/auth) and connecting a Google account
// (http/connect) share when they send the browser to Google and take it
// back: one random secret in a short-lived cookie, from which the state, the
// nonce and the PKCE verifier are derived. Each is a hash with its own label:
// seeing one (the state travels in a URL) reveals neither the secret nor the
// others. Each flow has its own purpose label, cookie name and cookie path,
// so neither's cookie or state counts for the other.
//
// A start may bind a note to its attempt: something that is no secret and
// that the callback must get back as the start set it (sign-in for the phone
// app binds the app's own PKCE challenge, D-095). The note travels in the
// cookie after the secret, and everything in the cookie is under the state:
// state, nonce and verifier are derived from the cookie's whole value.
#pragma once

#include "core/config.hpp"
#include "core/crypto.hpp"
#include "http/router.hpp"

#include <chrono>
#include <expected>
#include <format>
#include <optional>
#include <string>
#include <string_view>

namespace poni::http {

// The dashboard is served over HTTPS: its cookies are Secure.
inline bool served_over_https(const core::Config& config) { return config.public_url.starts_with("https://"); }

// The S256 challenge of a PKCE verifier (RFC 7636): the base64url of its SHA-256, without padding.
inline std::string s256_challenge(std::string_view verifier) { return core::base64url_encode(core::sha256_raw(verifier)); }

class BrowserFlow {
public:
    BrowserFlow(std::string_view purpose, std::string_view cookie_name, std::string_view cookie_path, bool secure)
        : purpose_(purpose), cookie_name_(cookie_name), cookie_path_(cookie_path), secure_(secure) {}

    std::string derive(std::string_view label, std::string_view secret) const {
        return core::base64url_encode(core::sha256_raw(std::format("{}:{}:{}", purpose_, label, secret)));
    }
    std::string verifier(std::string_view secret) const { return derive("verifier", secret); }
    // The PKCE challenge (S256) of the verifier.
    std::string code_challenge(std::string_view secret) const { return s256_challenge(verifier(secret)); }

    // The cookie's value for an attempt with a note: "<secret>.<note>". A
    // secret (core::random_token) holds no dot, so the first dot ends it. The
    // whole value is what secret() returns and what derive() hashes, so a
    // callback whose cookie holds another note, or none, derives another
    // state and fails as for any state that is not its own.
    static std::string with_note(std::string_view secret, std::string_view note) { return std::format("{}.{}", secret, note); }
    // The note in a cookie's value; nothing for an attempt that was started without one.
    static std::optional<std::string_view> note(std::string_view value) {
        const auto dot = value.find('.');
        if (dot == std::string_view::npos) return std::nullopt;
        return value.substr(dot + 1);
    }

    // Lax, not Strict: Google's redirect back is a cross-site navigation, and
    // a Strict cookie wouldn't be sent with it. An empty value with no age
    // clears it.
    std::string cookie(std::string_view value, std::chrono::seconds max_age) const {
        return std::format("{}={}; Path={}; Max-Age={}; HttpOnly; SameSite=Lax{}", cookie_name_, value, cookie_path_, max_age.count(),
                           secure_ ? "; Secure" : "");
    }

    // The secret in the browser's cookie, with the attempt's note if it has one; nothing if it sent none.
    std::optional<std::string> secret(const Request& request) const {
        auto value = cookie_value(request.header("cookie"), cookie_name_);
        if (!value || value->empty()) return std::nullopt;
        return value;
    }

    // The code Google sent the browser back with, if the state is the one
    // derived from `secret`: the state ties the answer to the browser that
    // started the flow. Otherwise why not: "cancelled" (refused at Google) or
    // "failed".
    std::expected<std::string, std::string_view> returned_code(const Request& request, std::string_view secret) const {
        const QueryParams query(request.query);
        if (query.get("error")) return std::unexpected("cancelled");
        const auto state = query.get("state");
        auto code = query.get("code");
        if (!state || !code || !core::constant_time_equal(*state, derive("state", secret))) return std::unexpected("failed");
        return std::move(*code);
    }

private:
    std::string_view purpose_;
    std::string_view cookie_name_;
    std::string_view cookie_path_;
    bool secure_;
};

}  // namespace poni::http
