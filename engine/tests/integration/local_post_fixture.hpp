// Shared by the integration tests about local posts (FR-14, D-088): two
// linked locations, a staff member, a Google that holds a new post in
// processing, and the jobs.
#pragma once

#include "db/repo_local_posts.hpp"
#include "engine_fixture.hpp"
#include "jobs/local_posts.hpp"

#include <optional>
#include <string>

namespace poni::test {

// The same locations as kSpa and kBuffet, as C strings for the helpers that take one.
inline constexpr const char* kSpaPosts = "9001";
inline constexpr const char* kBuffetPosts = "9002";
// Where the tests' public bucket would be.
inline constexpr std::string_view kMediaBase = "https://storage.googleapis.com/poni-public-test/";

class LocalPostTest : public EngineTest {
protected:
    void SetUp() override {
        EngineTest::SetUp();
        spa_and_buffet();
        add_staff();
        local_posts_.emplace(db(), clock_, google_, google_, queue(), jobs::LocalPostOptions{.media_base = std::string(kMediaBase)});
        jobs::install(registry_, *local_posts_, false);
    }

    void TearDown() override {
        local_posts_.reset();
        EngineTest::TearDown();
    }

    // Reads one location's posts from "Google".
    repo::LocalPostsSyncOutcome sync(std::int64_t location) { return await(local_posts_->sync(location)); }

    // A column of a post, as text ("" for NULL).
    std::string post(std::int64_t id, const std::string& column) {
        return value<std::string>("SELECT coalesce(" + column + "::text, '') FROM local_posts WHERE id = " + std::to_string(id));
    }
    std::int64_t post_named(const std::string& google_name) {
        return value<std::int64_t>("SELECT id FROM local_posts WHERE google_name = '" + google_name + "'");
    }
    static std::string name_of(const char* location, int post) { return std::format("accounts/111/locations/{}/localPosts/{}", location, post); }

    long audited(const std::string& action) { return count("audit_log WHERE action = '" + action + "'"); }

    FakeLocalPosts google_;
    std::optional<jobs::LocalPosts> local_posts_;
};

}  // namespace poni::test
