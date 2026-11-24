// The users as the API lists them: every User says its status, and the list
// shows the active (the default), the disabled or both. Nothing here changes
// a status through the API; a test disables a user in its own throwaway
// database, as the staff check will.

#include "api_fixture.hpp"

#include <format>

using namespace poni;

namespace {

constexpr std::string_view kColleague = "other@xvisionmediaus.com";

class UsersApi : public test::ApiTest {
protected:
    void SetUp() override {
        test::ApiTest::SetUp();
        browser_ = sign_in();
    }

    void disable(std::string_view email) { exec(std::format("UPDATE users SET status = 'disabled' WHERE email = '{}'", email)); }

    // The addresses a list of users holds, in its order.
    std::vector<std::string> emails(const std::string& target) {
        std::vector<std::string> out;
        for (const auto& user : read(target).at("items").as_array()) out.emplace_back(user.at("email").as_string());
        return out;
    }
};

}  // namespace

TEST_F(UsersApi, TheListIsByStatusAndTheDefaultIsWhoCanBeAssigned) {
    sign_in(staff(std::string(kColleague)));
    const auto before = read("/api/users").at("items").as_array();
    ASSERT_EQ(before.size(), 2u);
    for (const auto& user : before) {
        EXPECT_EQ(user.as_object().size(), 5u) << "id, email, name, picture_url and status";
        EXPECT_EQ(user.at("status").as_string(), "active");
    }
    EXPECT_TRUE(read("/api/users?status=disabled").at("items").as_array().empty());

    disable(kColleague);
    EXPECT_EQ(emails("/api/users"), std::vector<std::string>{"staff@xvisionmediaus.com"});
    EXPECT_EQ(emails("/api/users?status=active"), std::vector<std::string>{"staff@xvisionmediaus.com"});
    EXPECT_EQ(emails("/api/users?status=disabled"), std::vector<std::string>{std::string(kColleague)});
    EXPECT_EQ(read("/api/users?status=disabled").at("items").at(0).at("status").as_string(), "disabled");
    EXPECT_EQ(emails("/api/users?status=all").size(), 2u);
    const auto odd = get("/api/users?status=odd", browser_);
    EXPECT_EQ(odd.status, 400u);
    EXPECT_EQ(error_code(odd), "invalid_request");
    EXPECT_EQ(count("audit_log WHERE action LIKE 'user.%'"), 0) << "a list changes nothing";
}

TEST_F(UsersApi, TheSessionsUserIsActive) {
    EXPECT_EQ(read("/api/session").at("user").at("status").as_string(), "active");
    const auto answer = exchange(app_code(app_sign_in_response(staff(std::string(kColleague)))));
    ASSERT_EQ(answer.status, 200u) << answer.body;
    EXPECT_EQ(json(answer).at("session").at("user").at("status").as_string(), "active");
}
