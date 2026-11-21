// The two kinds of session (D-040, D-095): the dashboard's, which a browser
// holds as a cookie, and the phone app's, which it sends as a bearer token.
// A session is of one kind for good and is accepted only the way its kind
// arrives: a token is never taken as a cookie, a cookie's value never as a
// token. Each kind has its own lifetime.
#pragma once

#include <chrono>
#include <string_view>

namespace poni::domain {

enum class SessionKind { browser, app };

// As the sessions table and the audit log write it.
constexpr std::string_view name(SessionKind kind) { return kind == SessionKind::app ? "app" : "browser"; }

// How long a session lives: `total` after sign-in at the most, and `idle`
// after its last use.
struct SessionLifetime {
    std::chrono::seconds total;
    std::chrono::seconds idle;
};

// Each kind's own lifetime. The values here are the decided ones; the engine
// runs with what its configuration says (http::session_lifetimes).
struct SessionLifetimes {
    SessionLifetime browser{std::chrono::hours(24 * 7), std::chrono::hours(24)};
    SessionLifetime app{std::chrono::hours(24 * 90), std::chrono::hours(24 * 30)};

    constexpr const SessionLifetime& of(SessionKind kind) const { return kind == SessionKind::app ? app : browser; }
};

}  // namespace poni::domain
