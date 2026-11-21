// The phone app's devices, for push (D-097). A device is the APNs token an
// app session gave, and lives as long as that session: signing out, a
// session that ends and a user who is disabled stop its pushes, with
// nothing here to do it. Blocking calls on a transaction the caller owns.
#pragma once

#include "core/clock.hpp"
#include "core/config.hpp"
#include "db/audit.hpp"
#include "db/failure.hpp"
#include "domain/models.hpp"
#include "domain/session.hpp"

#include <boost/describe/class.hpp>
#include <pqxx/pqxx>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace poni::repo {

// The body of POST /api/devices, member for member.
struct NewDevice {
    std::string apns_token;
    std::string environment;  // "production" or "sandbox"
    std::string app_version;
};
BOOST_DESCRIBE_STRUCT(NewDevice, (), (apns_token, environment, app_version))

// Stores the token for the app session `session_token_hash`, which the
// caller has checked is an app session: one device per session, and a
// token another session had moves to this one. `engine` is the one APNs
// environment the engine sends to, nothing without the APNs settings; a
// token of another is refused. A registration that changes nothing writes
// nothing.
Result<model::Device> register_device(pqxx::transaction_base& tx, audit::Writer& audit, std::string_view session_token_hash,
                                      const NewDevice& device, std::optional<core::ApnsEnvironment> engine);

// Removes a device of `user_id`'s; another user's is not found.
Result<void> unregister_device(pqxx::transaction_base& tx, audit::Writer& audit, std::int64_t user_id, std::int64_t id);

struct PushDevice {
    std::int64_t id = 0;
    std::string apns_token;
};

// The devices push reaches for the staff member `email` now: those of app
// sessions that have not ended, of a user who is active.
std::vector<PushDevice> push_devices(pqxx::transaction_base& tx, std::string_view email, core::TimePoint now,
                                     const domain::SessionLifetimes& lifetimes);

// Apple said the token will never work again. Only that token: the device
// may have registered a new one since.
void drop_device(pqxx::transaction_base& tx, std::int64_t id, std::string_view apns_token);

}  // namespace poni::repo
