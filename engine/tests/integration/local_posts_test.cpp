// Local posts over a real PostgreSQL (FR-14, D-088): what the table holds
// and refuses, and how the API shows a post.

#include "api_fixture.hpp"
#include "db/repo_local_posts.hpp"
#include "domain/video_info.hpp"
#include "video_bytes.hpp"

#include <gmock/gmock.h>

using namespace poni;
using namespace std::chrono_literals;

namespace {

class LocalPosts : public test::ApiTest {
protected:
    void SetUp() override {
        api_public_store_ = &store_;
        ApiTest::SetUp();
        browser_ = sign_in();
        spa_and_buffet();
        user_ = value<std::int64_t>("SELECT id FROM users WHERE email = 'staff@xvisionmediaus.com'");
    }

    test::FakeStore store_{"https://storage.googleapis.com/poni-public-test/"};



    std::int64_t draft(std::int64_t location, const std::string& summary) {
        return insert_local_post(location, "origin, status, content, created_by",
                                 std::format("'system', 'draft', {}, {}", test::content_sql(test::standard(summary)), user_));
    }

    // Whether the table takes the row.
    bool accepted(const std::string& columns, const std::string& values) {
        try {
            insert_local_post(spa_, columns, values);
            return true;
        } catch (const pqxx::check_violation&) {
            return false;
        }
    }

    // A post of the spa in `status`, with what the table requires of that status.
    std::int64_t in_status(const std::string& status, const std::string& summary = "A post.") {
        const auto content = test::content_sql(test::standard(summary));
        const auto who = std::to_string(user_);
        if (status == "draft" || status == "rejected") {
            return insert_local_post(spa_, "origin, status, content, created_by, decided_by",
                                     std::format("'system', '{}', {}, {}, {}", status, content, who, status == "rejected" ? who : "NULL"));
        }
        if (status == "approved" || status == "sending" || status == "unknown" || status == "failed") {
            return insert_local_post(spa_, "origin, status, content, created_by, decided_by, approved_content, send_started_at",
                                     std::format("'system', '{}', {}, {}, {}, {}, '2026-11-01T00:00:00Z'", status, content, who, who, content));
        }
        return insert_local_post(spa_, "origin, status, content, created_by, decided_by, approved_content, google_name, google_state, sent_at",
                                 std::format("'system', '{}', {}, {}, {}, {}, 'accounts/111/locations/9001/localPosts/{}', 'live', "
                                             "'2026-11-01T00:00:00Z'",
                                             status, content, who, who, content, ++google_ids_));
    }
    int google_ids_ = 500;
    std::string status_of(std::int64_t id) { return value<std::string>(std::format("SELECT status FROM local_posts WHERE id = {}", id)); }

    std::vector<std::int64_t> ids(const std::string& query) {
        std::vector<std::int64_t> out;
        for (const auto& item : read("/api/local-posts" + query).at("items").as_array()) out.push_back(item.at("id").as_int64());
        return out;
    }

};

}  // namespace

// ---- reading -------------------------------------------------------------------------

TEST_F(LocalPosts, ADraftIsShownWithItsContentAndItsAuthor) {
    const auto id = draft(spa_, "Fall specials start Monday.");
    const auto post = read("/api/local-posts/" + std::to_string(id));
    EXPECT_EQ(post.at("location_id").as_int64(), spa_);
    EXPECT_EQ(post.at("location_title").as_string(), "LU DREAM SPA - Chelsea");
    EXPECT_EQ(post.at("client_id").as_int64(), spa_client_);
    EXPECT_EQ(post.at("client_name").as_string(), "LU DREAM SPA");
    EXPECT_EQ(post.at("origin").as_string(), "system");
    EXPECT_EQ(post.at("status").as_string(), "draft");
    EXPECT_EQ(post.at("topic_type").as_string(), "standard");
    EXPECT_EQ(post.at("summary").as_string(), "Fall specials start Monday.");
    EXPECT_EQ(post.at("language_code").as_string(), "en");
    EXPECT_EQ(post.at("created_by").as_string(), "staff@xvisionmediaus.com");
    for (const auto* nothing : {"status_reason", "google_state", "google_name", "search_url", "call_to_action", "event", "offer",
                                "media_id", "scheduled_time", "edited_by", "decided_by", "decided_at", "sent_at",
                                "removal_requested_by", "removal_requested_at", "removed_at", "google_created_at", "synced_at"}) {
        EXPECT_TRUE(post.at(nothing).is_null()) << nothing;
    }
    EXPECT_TRUE(post.at("media").as_array().empty());
    EXPECT_TRUE(post.at("checks").as_array().empty());

    EXPECT_EQ(get("/api/local-posts/999999", browser_).status, 404u);
    EXPECT_EQ(get("/api/local-posts/x", browser_).status, 404u);
    EXPECT_EQ(get("/api/local-posts/" + std::to_string(id)).status, 401u) << "nobody reads a post without signing in";
    EXPECT_EQ(get("/api/local-posts").status, 401u);
}

TEST_F(LocalPosts, AnOfferCarriesItsEventItsRepeatAndItsThreeOfferFields) {
    auto content = test::standard("Ten percent off lunch.");
    content.topic_type = "offer";
    content.event = domain::LocalPostEvent{.title = "Lunch week",
                                           .schedule = {"2026-12-19", "11:00", "2026-12-26", "15:30"},
                                           .recurrence = domain::LocalPostRecurrence{"weekly", "2027-01-31T00:00:00.000000Z"}};
    content.offer = domain::LocalPostOffer{.coupon_code = "LUNCH10", .redeem_online_url = std::nullopt, .terms_conditions = "Dine-in only."};
    content.scheduled_time = "2026-12-18T17:00:00.000000Z";
    const auto id = insert_local_post(buffet_, "origin, status, content, created_by",
                                      std::format("'system', 'draft', {}, {}", test::content_sql(content), user_));
    const auto post = read("/api/local-posts/" + std::to_string(id));
    EXPECT_EQ(post.at("topic_type").as_string(), "offer");
    const auto& event = post.at("event").as_object();
    EXPECT_EQ(event.at("title").as_string(), "Lunch week");
    EXPECT_EQ(event.at("schedule").at("start_date").as_string(), "2026-12-19");
    EXPECT_EQ(event.at("schedule").at("start_time").as_string(), "11:00");
    EXPECT_EQ(event.at("schedule").at("end_date").as_string(), "2026-12-26");
    EXPECT_EQ(event.at("schedule").at("end_time").as_string(), "15:30");
    EXPECT_EQ(event.at("recurrence").at("frequency").as_string(), "weekly");
    EXPECT_EQ(event.at("recurrence").at("until").as_string(), "2027-01-31T00:00:00.000000Z");
    const auto& offer = post.at("offer").as_object();
    EXPECT_EQ(offer.at("coupon_code").as_string(), "LUNCH10");
    EXPECT_TRUE(offer.at("redeem_online_url").is_null()) << "an offer has all three fields, set or not";
    EXPECT_EQ(offer.at("terms_conditions").as_string(), "Dine-in only.");
    EXPECT_EQ(post.at("scheduled_time").as_string(), "2026-12-18T17:00:00.000000Z");
    EXPECT_TRUE(post.at("call_to_action").is_null());
}

TEST_F(LocalPosts, APostMadeAtGoogleShowsWhatGoogleHasIncludingSeveralPhotos) {
    auto content = test::standard("Now open late on Fridays.");
    content.call_to_action = domain::LocalPostCallToAction{.action_type = "call", .url = std::nullopt};
    const auto id = insert_local_post(
        buffet_,
        "origin, status, content, google_name, google_state, search_url, google_media, "
        "google_created_at, google_updated_at, synced_at, raw",
        "'google', 'sent', " + test::content_sql(content) +
            R"(, 'accounts/111/locations/9002/localPosts/777', 'live', 'https://local.google.com/place?id=1&use=posts',
           '[{"format": "photo", "url": "https://lh3.googleusercontent.com/p/a"},
             {"format": "photo", "url": "https://lh3.googleusercontent.com/p/b"},
             {"format": "photo", "url": "https://lh3.googleusercontent.com/p/c"}]',
           '2026-11-04T03:16:22Z', '2026-11-04T03:16:47Z', '2026-11-19T10:00:00Z', '{"state": "LIVE"}')");
    const auto post = read("/api/local-posts/" + std::to_string(id));
    EXPECT_EQ(post.at("origin").as_string(), "google");
    EXPECT_EQ(post.at("status").as_string(), "sent");
    EXPECT_EQ(post.at("google_state").as_string(), "live");
    EXPECT_EQ(post.at("google_name").as_string(), "accounts/111/locations/9002/localPosts/777");
    EXPECT_TRUE(post.at("created_by").is_null());
    EXPECT_EQ(post.at("call_to_action").at("action_type").as_string(), "call");
    EXPECT_TRUE(post.at("call_to_action").at("url").is_null());
    ASSERT_EQ(post.at("media").as_array().size(), 3u) << "Google's own interface allows several photos";
    EXPECT_EQ(post.at("media").as_array()[2].at("url").as_string(), "https://lh3.googleusercontent.com/p/c");
    EXPECT_TRUE(post.at("media_id").is_null());
    EXPECT_FALSE(post.as_object().contains("raw")) << "the payload stays in the database";
}

TEST_F(LocalPosts, ADraftsUploadedPhotoIsShownFromThePublicBucket) {
    const auto media = insert_upload({});
    auto content = test::standard("New room.");
    content.media_id = media;
    const auto id = insert_local_post(spa_, "origin, status, content", "'system', 'draft', " + test::content_sql(content));
    const auto post = read("/api/local-posts/" + std::to_string(id));
    EXPECT_EQ(post.at("media_id").as_int64(), media);
    ASSERT_EQ(post.at("media").as_array().size(), 1u);
    EXPECT_EQ(post.at("media").as_array()[0].at("format").as_string(), "photo");
    EXPECT_EQ(post.at("media").as_array()[0].at("url").as_string(), "https://storage.googleapis.com/poni-public-test/p/abc123.jpg");
}

TEST_F(LocalPosts, TheListIsNewestFirstAndFiltersByClientLocationAndState) {
    const auto old_google = insert_local_post(buffet_, "origin, status, content, google_name, google_state, google_created_at",
                                              "'google', 'sent', " + test::content_sql(test::standard("Summer night.")) +
                                                  ", 'accounts/111/locations/9002/localPosts/1', 'live', '2026-08-28T22:40:31Z'");
    const auto spa_draft = draft(spa_, "First.");
    clock_.advance(1min);
    const auto buffet_draft = draft(buffet_, "Second.");
    // Found by a later sync, but older at Google than both drafts: it sorts by Google's time.
    EXPECT_EQ(ids(""), (std::vector<std::int64_t>{buffet_draft, spa_draft, old_google}));
    EXPECT_EQ(read("/api/local-posts").at("total").as_int64(), 3);

    EXPECT_EQ(ids("?client_id=" + std::to_string(buffet_client_)), (std::vector<std::int64_t>{buffet_draft, old_google}));
    EXPECT_EQ(ids("?location_id=" + std::to_string(spa_)), (std::vector<std::int64_t>{spa_draft}));
    EXPECT_EQ(ids("?status=draft"), (std::vector<std::int64_t>{buffet_draft, spa_draft}));
    EXPECT_EQ(ids("?status=draft&client_id=" + std::to_string(spa_client_)), (std::vector<std::int64_t>{spa_draft}));
    EXPECT_EQ(ids("?per_page=1&page=2"), (std::vector<std::int64_t>{spa_draft}));
    EXPECT_EQ(read("/api/local-posts?per_page=1&page=2").at("total").as_int64(), 3);

    for (const auto* bad : {"?status=posted", "?client_id=x", "?per_page=0"}) {
        EXPECT_EQ(get(std::string("/api/local-posts") + bad, browser_).status, 400u) << bad;
    }
}

TEST_F(LocalPosts, APostOfALocationThatWasUnlinkedIsStillShown) {
    const auto id = draft(spa_, "Still here.");
    exec(std::format("UPDATE locations SET client_id = NULL WHERE id = {}", spa_));
    const auto post = read("/api/local-posts/" + std::to_string(id));
    EXPECT_TRUE(post.at("client_id").is_null());
    EXPECT_TRUE(post.at("client_name").is_null());
    EXPECT_EQ(read("/api/local-posts").at("total").as_int64(), 1);
}

// ---- writing, editing, approving and rejecting a draft ------------------------------------

namespace {

constexpr const char* kStandard = R"({"topic_type": "standard", "summary": "Fall specials start Monday.", "language_code": "en",
    "call_to_action": {"action_type": "learn_more", "url": "https://example.com/fall"}, "event": null, "offer": null,
    "media_id": null, "scheduled_time": "2026-12-19T17:00:00Z"})";

// The content fields of a post as the API returned it: what a person approves.
std::string content_of(const boost::json::value& post) {
    boost::json::object content;
    for (const auto* field : {"topic_type", "summary", "language_code", "call_to_action", "event", "offer", "media_id", "scheduled_time"}) {
        content[field] = post.at(field);
    }
    return boost::json::serialize(content);
}

}  // namespace

TEST_F(LocalPosts, ADraftIsWrittenEditedAndApprovedThroughTheApi) {
    exec("UPDATE system_settings SET posting_enabled = true");
    const auto created = post(std::format("/api/locations/{}/local-posts", spa_), kStandard);
    ASSERT_EQ(created.status, 201u) << created.body;
    const auto id = json(created).at("id").as_int64();
    const auto target = std::format("/api/local-posts/{}", id);
    EXPECT_EQ(json(created).at("status").as_string(), "draft");
    EXPECT_EQ(json(created).at("origin").as_string(), "system");
    EXPECT_EQ(json(created).at("created_by").as_string(), "staff@xvisionmediaus.com");
    EXPECT_EQ(json(created).at("scheduled_time").as_string(), "2026-12-19T17:00:00.000000Z");
    EXPECT_EQ(json(created).at("checks").as_array().size(), 12u);
    for (const auto& check : json(created).at("checks").as_array()) EXPECT_TRUE(check.at("passed").as_bool()) << check.at("name").as_string();

    // An edit replaces the whole content; a failed check is saved with it and says what was found.
    auto with_phone = boost::json::parse(kStandard).as_object();
    with_phone["summary"] = "Call (212) 555-0100 to book.";
    const auto edited = put(target, boost::json::serialize(with_phone));
    ASSERT_EQ(edited.status, 200u) << edited.body;
    EXPECT_EQ(json(edited).at("edited_by").as_string(), "staff@xvisionmediaus.com");
    bool phone_failed = false;
    for (const auto& check : json(edited).at("checks").as_array()) {
        if (check.at("name").as_string() == "no_phone") phone_failed = !check.at("passed").as_bool() && !check.at("detail").as_string().empty();
    }
    EXPECT_TRUE(phone_failed);
    const auto blocked = post(target + "/approve", content_of(json(edited)));
    EXPECT_EQ(blocked.status, 409u);
    EXPECT_EQ(error_code(blocked), "checks_failed");

    // Fixed; then the approval carries the content the person read.
    const auto fixed = put(target, kStandard);
    ASSERT_EQ(fixed.status, 200u);
    const auto stale = post(target + "/approve", boost::json::serialize(with_phone));
    EXPECT_EQ(stale.status, 409u);
    EXPECT_EQ(error_code(stale), "local_post_changed");
    const auto approved = post(target + "/approve", content_of(json(fixed)));
    ASSERT_EQ(approved.status, 202u) << approved.body;
    EXPECT_EQ(json(approved).at("status").as_string(), "approved");
    EXPECT_EQ(json(approved).at("decided_by").as_string(), "staff@xvisionmediaus.com");
    EXPECT_EQ(count("jobs WHERE kind = 'local_posts.publish'"), 1);

    EXPECT_EQ(error_code(put(target, kStandard)), "local_post_not_open");
    EXPECT_EQ(error_code(post(target + "/reject", "")), "local_post_not_open");

    // The post's page shows who did what, newest first.
    const auto log = read(std::format("/api/audit-log?object_type=local_post&object_id={}", id)).at("items").as_array();
    ASSERT_EQ(log.size(), 4u);
    EXPECT_EQ(log[0].at("action").as_string(), "local_post.approve");
    EXPECT_EQ(log[0].at("after").at("content").at("summary").as_string(), "Fall specials start Monday.") << "what was approved is kept";
    EXPECT_EQ(log[3].at("action").as_string(), "local_post.create");
    EXPECT_EQ(log[1].at("actor").as_string(), "staff@xvisionmediaus.com");
}

TEST_F(LocalPosts, ADraftIsRejectedWithOrWithoutAReason) {
    const auto first = json(post(std::format("/api/locations/{}/local-posts", spa_), kStandard)).at("id").as_int64();
    const auto second = json(post(std::format("/api/locations/{}/local-posts", spa_), kStandard)).at("id").as_int64();
    const auto rejected = post(std::format("/api/local-posts/{}/reject", first), R"({"reason": "Wrong week."})");
    ASSERT_EQ(rejected.status, 200u) << rejected.body;
    EXPECT_EQ(json(rejected).at("status").as_string(), "rejected");
    EXPECT_EQ(json(rejected).at("status_reason").as_string(), "Wrong week.");
    const auto silent = post(std::format("/api/local-posts/{}/reject", second), "");
    ASSERT_EQ(silent.status, 200u) << silent.body;
    EXPECT_TRUE(json(silent).at("status_reason").is_null());
    EXPECT_EQ(post(std::format("/api/local-posts/{}/reject", second), R"({"reason": 5})").status, 400u);
    EXPECT_EQ(post("/api/local-posts/999999/reject", "").status, 404u);
}

TEST_F(LocalPosts, WhatIsNotAPostIsRefusedWithAMessageThatSaysWhy) {
    const auto target = std::format("/api/locations/{}/local-posts", spa_);
    const auto refused = [&](std::string body) {
        const auto response = post(target, std::move(body));
        EXPECT_EQ(response.status, 400u) << response.body;
        return response.status == 400 ? std::string(json(response).at("error").at("message").as_string()) : std::string{};
    };
    // The content travels whole (D-092): kStandard with these members in place of its own.
    const auto with = [&](std::string_view members) {
        auto content = boost::json::parse(kStandard).as_object();
        for (const auto& member : boost::json::parse(members).as_object()) content[member.key()] = member.value();
        return refused(boost::json::serialize(content));
    };
    EXPECT_THAT(refused("[]"), testing::HasSubstr("JSON object"));
    EXPECT_THAT(refused(R"({"summary": "No kind.", "language_code": "en"})"), testing::HasSubstr("topic_type"));
    EXPECT_THAT(with(R"({"topic_type": "alert"})"), testing::HasSubstr("topic_type"));
    EXPECT_THAT(with(R"({"topic_type": "event", "event": {"title": "No dates"}})"), testing::HasSubstr("event.schedule"));
    EXPECT_THAT(with(R"({"scheduled_time": "next Friday"})"), testing::HasSubstr("scheduled_time"));
    EXPECT_THAT(with(R"({"media_id": 42})"), testing::HasSubstr("media_id"));
    EXPECT_EQ(count("local_posts"), 0);

    EXPECT_EQ(post("/api/locations/999999/local-posts", kStandard).status, 404u);
    EXPECT_EQ(call(verb::post, target, {}, kStandard).status, 401u);
    EXPECT_EQ(put("/api/local-posts/999999", kStandard).status, 404u);
    EXPECT_EQ(post("/api/local-posts/999999/approve", kStandard).status, 404u);
}

TEST_F(LocalPosts, WithPostingSwitchedOffAnApprovalIsRefusedAndSaysSo) {
    const auto id = json(post(std::format("/api/locations/{}/local-posts", spa_), kStandard)).at("id").as_int64();
    const auto shown = read(std::format("/api/local-posts/{}", id));
    const auto refused = post(std::format("/api/local-posts/{}/approve", id), content_of(shown));
    EXPECT_EQ(refused.status, 409u);
    EXPECT_EQ(error_code(refused), "posting_disabled") << "posting is off after installation, for posts as for replies";
    EXPECT_EQ(count("jobs WHERE kind = 'local_posts.publish'"), 0);
    EXPECT_EQ(count("audit_log WHERE action = 'local_post.approve'"), 0) << "a refused request leaves no entry";
}

// ---- a post's photo (D-088 point 9) ---------------------------------------------------------

namespace {

// The first bytes of a PNG of that size, padded to `bytes`: enough for the engine, which reads what a file is
// from its header and stores the rest untouched.
std::string png(unsigned width, unsigned height, std::size_t bytes = 12 * 1024) {
    const auto be32 = [](unsigned n) {
        return std::string{static_cast<char>(n >> 24), static_cast<char>((n >> 16) & 0xFF), static_cast<char>((n >> 8) & 0xFF),
                           static_cast<char>(n & 0xFF)};
    };
    std::string out = "\x89PNG\r\n\x1a\n" + be32(13) + "IHDR" + be32(width) + be32(height) + std::string("\x08\x02\x00\x00\x00", 5);
    // A phone writes where and when into the file; whatever follows the header stays as it is.
    out += "GPS 40.7421,-73.9911";
    out.resize(std::max(bytes, out.size()), 'x');
    return out;
}

class LocalPostMedia : public LocalPosts {
protected:

    http::Response upload(const std::string& bytes, const test::Browser& browser) {
        return call(verb::post, "/api/local-post-media", browser, bytes, {{"content-type", "image/png"}});
    }
    http::Response upload(const std::string& bytes) { return upload(bytes, browser_); }
    // What the server asks before it reads the body of this route, with the real sign-in: the status of a
    // refusal, or nothing when the body may be read. Decided from the headers alone.
    std::optional<unsigned> before_body(std::map<std::string, std::string> headers) {
        http::Request request;
        request.method = verb::post;
        request.path = "/api/local-post-media";
        request.remote_ip = "203.0.113.10";
        request.headers["content-type"] = "video/mp4";
        for (auto& [name, value] : headers) request.headers[name] = std::move(value);
        const auto refused = await(router_->refuse_before_body(std::move(request)));
        return refused ? std::optional<unsigned>(refused->status) : std::nullopt;
    }
    std::optional<unsigned> before_body(const test::Browser& browser) {
        std::map<std::string, std::string> headers;
        if (!browser.cookie.empty()) headers["cookie"] = browser.cookie;
        if (!browser.csrf.empty()) headers["x-csrf-token"] = browser.csrf;
        return before_body(std::move(headers));
    }
    // A column of the uploaded file's row, as text; empty for NULL.
    std::string media_value(const boost::json::value& media, const std::string& column) {
        return value<std::string>(std::format("SELECT coalesce({}::text, '') FROM local_post_media WHERE id = {}", column, media.at("id").as_int64()));
    }
};

}  // namespace

TEST_F(LocalPostMedia, APhotoGoesIntoThePublicBucketExactlyAsItWasUploaded) {
    const auto bytes = png(1200, 900, 300 * 1024);
    const auto response = upload(bytes);
    ASSERT_EQ(response.status, 201u) << response.body;
    const auto media = json(response);
    EXPECT_EQ(media.at("format").as_string(), "photo");
    EXPECT_EQ(media_value(media, "content_type"), "image/png");
    EXPECT_EQ(media.at("bytes").as_int64(), 300 * 1024);
    EXPECT_EQ(media.at("width").as_int64(), 1200);
    EXPECT_EQ(media.at("height").as_int64(), 900);
    EXPECT_EQ(media.at("created_by").as_string(), "staff@xvisionmediaus.com");

    ASSERT_EQ(store_.objects.size(), 1u);
    const auto& [name, object] = *store_.objects.begin();
    EXPECT_THAT(name, testing::MatchesRegex("p/[A-Za-z0-9_-]{43}\\.png")) << "32 random bytes: a name nobody can guess";
    EXPECT_EQ(object.body, bytes) << "nothing is resized, converted or stripped";
    EXPECT_EQ(object.content_type, "image/png");
    EXPECT_EQ(media.at("url").as_string(), "https://storage.googleapis.com/poni-public-test/" + name);
    EXPECT_EQ(count("local_post_media"), 1);
    EXPECT_EQ(count("audit_log WHERE action = 'local_post_media.upload'"), 1);
    EXPECT_EQ(count(std::format("audit_log WHERE after::text LIKE '%{}%'", name.substr(2, 43))), 0) << "the name is in no audit entry";

    // Two uploads of the same file are two objects.
    ASSERT_EQ(upload(bytes).status, 201u);
    EXPECT_EQ(store_.objects.size(), 2u);

    // A draft takes the upload by its id, and shows it from the bucket.
    auto content = boost::json::parse(kStandard).as_object();
    content["media_id"] = media.at("id");
    const auto created = post(std::format("/api/locations/{}/local-posts", spa_), boost::json::serialize(content));
    ASSERT_EQ(created.status, 201u) << created.body;
    EXPECT_EQ(json(created).at("media").as_array()[0].at("url"), media.at("url"));
    for (const auto& check : json(created).at("checks").as_array()) EXPECT_TRUE(check.at("passed").as_bool()) << check.at("name").as_string();
}

TEST_F(LocalPostMedia, WhatGoogleWouldNotTakeIsRefusedAndNothingIsStored) {
    const auto refused = [&](const std::string& bytes) {
        const auto response = upload(bytes);
        EXPECT_EQ(response.status, 400u) << response.body;
        return response.status == 400 ? std::string(json(response).at("error").at("message").as_string()) : std::string{};
    };
    EXPECT_THAT(refused(std::string("GIF89a") + std::string(20000, 'x')), testing::HasSubstr("neither a JPG or PNG photo nor an MP4"));
    EXPECT_THAT(refused("<svg xmlns='http://www.w3.org/2000/svg'><script>alert(1)</script></svg>" + std::string(20000, ' ')),
                testing::HasSubstr("neither a JPG or PNG photo"))
        << "what a file is comes from its bytes, not from the Content-Type it was sent with";
    EXPECT_THAT(refused(""), testing::HasSubstr("neither a JPG or PNG photo"));
    EXPECT_THAT(refused(png(1200, 900, 5 * 1024)), testing::HasSubstr("10 KB"));
    EXPECT_THAT(refused(png(1200, 900, 5 * 1024 * 1024 + 1)), testing::HasSubstr("5 MB"));
    EXPECT_THAT(refused(png(1200, 249)), testing::HasSubstr("at least 250"));
    EXPECT_EQ(store_.puts, 0);
    EXPECT_EQ(count("local_post_media"), 0);

    EXPECT_EQ(upload(png(1200, 900), {}).status, 401u);
    EXPECT_EQ(upload(png(1200, 900), {.cookie = browser_.cookie, .csrf = ""}).status, 403u);
    EXPECT_EQ(store_.puts, 0);
}

// ---- a post's video (D-090) ---------------------------------------------------------------------

TEST_F(LocalPostMedia, AVideoGoesIntoThePublicBucketExactlyAsItWasUploaded) {
    // As a phone writes it: the movie box at the end, a sound track first, upright. The upload says
    // "image/png" (this fixture always does): what the file is comes from its bytes.
    const auto bytes = test::video_file(
        {.width = 1080, .height = 1920, .timescale = 600, .duration = 7500, .movie_box_last = true, .sound_track_first = true, .bytes = 2 * 1024 * 1024});
    const auto response = upload(bytes);
    ASSERT_EQ(response.status, 201u) << response.body;
    const auto media = json(response);
    EXPECT_EQ(media.at("format").as_string(), "video");
    EXPECT_EQ(media_value(media, "content_type"), "video/mp4");
    EXPECT_EQ(media.at("bytes").as_int64(), 2 * 1024 * 1024);
    EXPECT_EQ(media.at("width").as_int64(), 1080);
    EXPECT_EQ(media.at("height").as_int64(), 1920);
    EXPECT_EQ(media_value(media, "duration_ms"), "12500");

    ASSERT_EQ(store_.objects.size(), 1u);
    const auto& [name, object] = *store_.objects.begin();
    EXPECT_THAT(name, testing::MatchesRegex("p/[A-Za-z0-9_-]{43}\\.mp4"));
    EXPECT_EQ(object.body, bytes) << "nothing is converted, cut or stripped";
    EXPECT_EQ(object.content_type, "video/mp4");
    EXPECT_EQ(value<std::string>("SELECT format || ' ' || duration_ms FROM local_post_media"), "video 12500");
    EXPECT_EQ(value<std::string>("SELECT after->>'duration_ms' FROM audit_log WHERE action = 'local_post_media.upload'"), "12500");

    // A QuickTime file keeps its own name and type.
    const auto quicktime = upload(test::video_file({.brand = "qt  "}));
    ASSERT_EQ(quicktime.status, 201u) << quicktime.body;
    EXPECT_EQ(media_value(json(quicktime), "content_type"), "video/quicktime");
    EXPECT_THAT(std::string(json(quicktime).at("url").as_string()), testing::EndsWith(".mov"));

    // A photo has no length.
    EXPECT_EQ(media_value(json(upload(png(1200, 900))), "duration_ms"), "");

    // A draft takes the video by its id, as it takes a photo, and shows it as a video.
    auto content = boost::json::parse(kStandard).as_object();
    content["media_id"] = media.at("id");
    const auto created = post(std::format("/api/locations/{}/local-posts", spa_), boost::json::serialize(content));
    ASSERT_EQ(created.status, 201u) << created.body;
    EXPECT_EQ(json(created).at("media").as_array()[0].at("format").as_string(), "video");
    EXPECT_EQ(json(created).at("media").as_array()[0].at("url"), media.at("url"));
    for (const auto& check : json(created).at("checks").as_array()) EXPECT_TRUE(check.at("passed").as_bool()) << check.at("name").as_string();
}

TEST_F(LocalPostMedia, AVideoGoogleWouldNotTakeIsRefusedAndNothingIsStored) {
    const auto refused = [&](const std::string& bytes) {
        const auto response = upload(bytes);
        EXPECT_EQ(response.status, 400u) << response.body;
        return response.status == 400 ? std::string(json(response).at("error").at("message").as_string()) : std::string{};
    };
    EXPECT_THAT(refused(test::video_file({.duration = 31 * 600})), testing::HasSubstr("runs 31.0 seconds"));
    EXPECT_THAT(refused(test::video_file({.width = 960, .height = 540})), testing::HasSubstr("at least 720"));
    EXPECT_THAT(refused(test::video_file({.handler = "soun"})), testing::HasSubstr("not an MP4 or QuickTime video")) << "sound only";
    EXPECT_THAT(refused(test::video_file({.duration = 0})), testing::HasSubstr("length and size can be read"));
    EXPECT_THAT(refused(test::video_file().substr(0, 40)), testing::HasSubstr("not an MP4 or QuickTime video")) << "cut off";
    // A still image in a video's format (a phone's HEIC photo) is neither, and the person is told what it is.
    EXPECT_THAT(refused(std::string("\0\0\0\x18", 4) + "ftypheic" + std::string(12, '\0') + std::string(20000, 'x')),
                testing::HasSubstr("HEIC or AVIF picture"));
    EXPECT_THAT(refused(test::video_file({.fragmented = true})), testing::HasSubstr("length and size can be read"));
    // A JPEG's first bytes with a video's mark after them is neither a photo nor a video.
    EXPECT_THAT(refused(std::string("\xFF\xD8\xFF\xE0", 4) + "ftypisom" + std::string(20000, 'x')), testing::HasSubstr("not an MP4 or QuickTime video"));
    EXPECT_THAT(refused(test::video_file({.bytes = domain::kVideoMaxBytes + 1})), testing::HasSubstr("75 MB"));
    EXPECT_EQ(store_.puts, 0);
    EXPECT_EQ(count("local_post_media"), 0);
}

TEST_F(LocalPostMedia, OnlyASignedInPersonsFileIsReadAtAll) {
    // The same answers as the request itself would get.
    EXPECT_EQ(before_body(test::Browser{}), 401u) << "nobody";
    EXPECT_EQ(before_body(test::Browser{.cookie = browser_.cookie, .csrf = ""}), 403u) << "a page of another site, which has the cookie and not the token";
    EXPECT_EQ(before_body(browser_), std::nullopt) << "a signed-in person: the body may be read";
    EXPECT_EQ(router_->body_limit(verb::post, "/api/local-post-media"), domain::kVideoMaxBytes);
    EXPECT_EQ(router_->body_limit(verb::post, "/api/local-posts"), http::Router::kDefaultBodyLimit);
}

TEST_F(LocalPostMedia, TheAppUploadsWithItsToken) {
    // A signed-in app sends a post's photo (D-095). The check before the body knows the caller from the
    // Authorization header alone, as it knows a browser from its cookie.
    const auto token = app_sign_in(staff("phone@xvisionmediaus.com"));
    EXPECT_EQ(before_body(bearer(token)), std::nullopt) << "no CSRF token, and the body may be read";
    EXPECT_EQ(before_body(bearer("not-a-token")), 401u) << "nobody's token: the file is never read";

    const auto response = as_app(verb::post, "/api/local-post-media", token, png(1200, 900), {{"content-type", "image/png"}});
    ASSERT_EQ(response.status, 201u) << response.body;
    EXPECT_EQ(json(response).at("created_by").as_string(), "phone@xvisionmediaus.com");
    EXPECT_EQ(store_.puts, 1);
}

TEST_F(LocalPostMedia, AFileThatCouldNotBeStoredIsNotRecorded) {
    store_.refuse = true;
    auto failed = upload(png(1200, 900));
    EXPECT_EQ(failed.status, 502u);
    EXPECT_EQ(error_code(failed), "storage_failed");
    store_.refuse = false;
    store_.unreachable = true;
    failed = upload(png(1200, 900));
    EXPECT_EQ(failed.status, 502u);
    EXPECT_EQ(count("local_post_media"), 0);
    EXPECT_EQ(count("audit_log WHERE action = 'local_post_media.upload'"), 0);
}

TEST_F(LocalPosts, WithoutAPublicBucketAPostCanHaveNoPhoto) {
    api_public_store_ = nullptr;
    build({});
    browser_ = sign_in();
    const auto refused = call(verb::post, "/api/local-post-media", browser_, png(1200, 900), {{"content-type", "image/png"}});
    EXPECT_EQ(refused.status, 409u);
    EXPECT_EQ(error_code(refused), "no_public_bucket");
}

// ---- removing a post at Google --------------------------------------------------------------

TEST_F(LocalPosts, APostThatGoogleHasIsRemovedOnRequestAndADraftIsNot) {
    const auto at_google = insert_local_post(buffet_, "origin, status, content, google_name, google_state",
                                             "'google', 'sent', " + test::content_sql(test::standard("Made at Google.")) +
                                                 ", 'accounts/111/locations/9002/localPosts/777', 'live'");
    const auto target = std::format("/api/local-posts/{}/remove", at_google);
    const auto requested = post(target, "");
    ASSERT_EQ(requested.status, 202u) << requested.body;
    EXPECT_EQ(json(requested).at("removal_requested_by").as_string(), "staff@xvisionmediaus.com");
    EXPECT_FALSE(json(requested).at("removal_requested_at").is_null());
    EXPECT_EQ(json(requested).at("status").as_string(), "sent") << "until Google has deleted it";
    EXPECT_EQ(count("jobs WHERE kind = 'local_posts.remove'"), 1);
    EXPECT_EQ(post(target, "").status, 202u) << "asking again changes nothing";
    EXPECT_EQ(count("jobs WHERE kind = 'local_posts.remove'"), 1);
    const auto log = read(std::format("/api/audit-log?object_type=local_post&object_id={}", at_google)).at("items").as_array();
    ASSERT_EQ(log.size(), 1u);
    EXPECT_EQ(log[0].at("action").as_string(), "local_post.remove_requested");
    EXPECT_EQ(log[0].at("after").at("content").at("summary").as_string(), "Made at Google.") << "a copy of what is being deleted";

    const auto a_draft = draft(spa_, "Never sent.");
    const auto refused = post(std::format("/api/local-posts/{}/remove", a_draft), "");
    EXPECT_EQ(refused.status, 409u);
    EXPECT_EQ(error_code(refused), "local_post_not_at_google");
    EXPECT_EQ(post("/api/local-posts/999999/remove", "").status, 404u);
    EXPECT_EQ(call(verb::post, target).status, 401u);
    EXPECT_EQ(count("jobs WHERE kind = 'local_posts.remove'"), 1);
}

// ---- "Refresh" ---------------------------------------------------------------------------

TEST_F(LocalPosts, RefreshQueuesTheReadOfOneLocationNow) {
    const auto target = std::format("/api/locations/{}/local-posts/refresh", spa_);
    const auto queued = post(target, "");
    ASSERT_EQ(queued.status, 202u) << queued.body;
    EXPECT_TRUE(json(queued).at("enqueued").as_bool());
    EXPECT_EQ(count(std::format("jobs WHERE kind = 'local_posts.sync' AND (payload->>'location_id')::bigint = {}", spa_)), 1);
    EXPECT_EQ(post(target, "").status, 202u);
    EXPECT_EQ(count("jobs WHERE kind = 'local_posts.sync'"), 2) << "pressed twice, read twice: a read changes nothing at Google";
    const auto log = read("/api/audit-log?action=local_posts.refresh").at("items").as_array();
    ASSERT_EQ(log.size(), 2u);
    EXPECT_EQ(log[0].at("actor").as_string(), "staff@xvisionmediaus.com");
    EXPECT_EQ(log[0].at("object_id").as_string(), std::to_string(spa_));

    EXPECT_EQ(post("/api/locations/999999/local-posts/refresh", "").status, 404u);
    EXPECT_EQ(call(verb::post, target).status, 401u);
    EXPECT_EQ(call(verb::post, target, {.cookie = browser_.cookie, .csrf = ""}).status, 403u) << "a change needs the CSRF token";

    // A location that is not linked to a client is not read.
    exec(std::format("UPDATE locations SET client_id = NULL WHERE id = {}", buffet_));
    const auto unlinked = post(std::format("/api/locations/{}/local-posts/refresh", buffet_), "");
    EXPECT_EQ(unlinked.status, 409u);
    EXPECT_EQ(error_code(unlinked), "location_not_synced");
    EXPECT_EQ(count("jobs WHERE kind = 'local_posts.sync'"), 2);
}

TEST_F(LocalPosts, AgainstTheGoogleMockNothingAsksGoogleAboutPosts) {
    api_options_.google_mock = true;
    build({});
    browser_ = sign_in();
    const auto refused = post(std::format("/api/locations/{}/local-posts/refresh", spa_), "");
    EXPECT_EQ(refused.status, 409u);
    EXPECT_EQ(error_code(refused), "local_posts_unavailable");
    EXPECT_EQ(count("jobs WHERE kind = 'local_posts.sync'"), 0);
    // No draft is written and none approved either: the mock could not take the post.
    const auto id = draft(spa_, "Written before the mock.");
    EXPECT_EQ(error_code(post(std::format("/api/locations/{}/local-posts", spa_), kStandard)), "local_posts_unavailable");
    EXPECT_EQ(error_code(post(std::format("/api/local-posts/{}/approve", id), kStandard)), "local_posts_unavailable");
    EXPECT_EQ(error_code(post(std::format("/api/local-posts/{}/remove", id), "")), "local_posts_unavailable");
    EXPECT_EQ(error_code(post(std::format("/api/local-posts/{}/copy", id), "")), "local_posts_unavailable") << "as writing a draft is";
    EXPECT_EQ(error_code(post("/api/local-post-media", png(1200, 900))), "local_posts_unavailable");
    EXPECT_EQ(count("local_posts"), 1);
    // What the engine holds can still be read.
    EXPECT_EQ(get("/api/local-posts", browser_).status, 200u);
}

TEST_F(LocalPosts, ALocationSaysWhenItsPostsWereLastRead) {
    const auto before = read(std::format("/api/locations/{}", spa_));
    EXPECT_TRUE(before.at("local_posts_synced_at").is_null());
    EXPECT_TRUE(before.at("local_posts_sync_error").is_null());
    exec(std::format("UPDATE locations SET local_posts_synced_at = '2026-11-10T12:00:00Z', local_posts_sync_error = 'HTTP 503' WHERE id = {}", spa_));
    const auto after = read(std::format("/api/locations/{}", spa_));
    EXPECT_EQ(after.at("local_posts_synced_at").as_string(), "2026-11-10T12:00:00.000000Z");
    EXPECT_EQ(after.at("local_posts_sync_error").as_string(), "HTTP 503");
}

// ---- what the table itself refuses ------------------------------------------------------

TEST_F(LocalPosts, NothingIsOnItsWayToGoogleWithoutAnApprovalOfItsContent) {
    const std::string columns = "origin, content, status";
    const std::string content = "'system', " + test::content_sql(test::standard("Hello."));
    EXPECT_TRUE(accepted(columns, content + ", 'draft'"));
    EXPECT_FALSE(accepted(columns, content + ", 'approved'")) << "an approved post names who approved it and what";
    EXPECT_FALSE(accepted(columns, content + ", 'sending'"));
    EXPECT_FALSE(accepted(columns + ", decided_by", std::format("{}, 'approved', {}", content, user_)))
        << "the approver alone is not enough: the content approved is kept";
    EXPECT_TRUE(accepted(columns + ", decided_by, approved_content", std::format("{}, 'approved', {}, '{{}}'", content, user_)));
    EXPECT_FALSE(accepted(columns + ", decided_by, approved_content", std::format("{}, 'sending', {}, '{{}}'", content, user_)))
        << "a send that was claimed says when";
    EXPECT_FALSE(accepted(columns, content + ", 'unknown'"));
    EXPECT_FALSE(accepted(columns, content + ", 'posted'")) << "not one of the states";
}

TEST_F(LocalPosts, TheTableKeepsAPostsPartsTogether) {
    // The content is one value (D-091 point 13): a button, an event and a repeat are each whole or absent in it.
    // What the table still holds it to: a kind, the one spelling of a time, and an upload that exists.
    const std::string base = "origin, status, content";
    auto event = test::standard("Hello.");
    event.topic_type = "event";
    event.event = domain::LocalPostEvent{.title = "Open house",
                                         .schedule = {"2026-12-19", "10:00", "2026-12-19", "16:00"},
                                         .recurrence = domain::LocalPostRecurrence{"weekly", "2027-01-31T00:00:00.000000Z"}};
    event.scheduled_time = "2026-12-18T17:00:00.000000Z";
    const std::string draft_values = "'system', 'draft', " + test::content_sql(event);
    EXPECT_TRUE(accepted(base, draft_values));
    EXPECT_FALSE(accepted(base, R"('system', 'draft', '{"summary": "Hello.", "language_code": "en"}')")) << "a post has a kind";
    EXPECT_FALSE(accepted(base, R"('system', 'draft', '{"topic_type": "product", "summary": "Hello.", "language_code": "en"}')"))
        << "one the engine knows";
    EXPECT_FALSE(accepted(base, R"('system', 'draft', '{"topic_type": "standard", "language_code": "en"}')")) << "and a text, set or empty";
    EXPECT_FALSE(accepted(base, R"('system', 'draft', '{"topic_type": "standard", "summary": "Hello.", "language_code": 5}')"));
    EXPECT_FALSE(accepted(base, "'system', 'draft', 'null'"));
    auto respelled = event;
    respelled.scheduled_time = "2026-12-18T17:00:00Z";
    EXPECT_FALSE(accepted(base, "'system', 'draft', " + test::content_sql(respelled))) << "times are compared as text: one spelling";
    respelled = event;
    respelled.event->recurrence->until = "2027-01-31T00:00:60.000000Z";
    EXPECT_FALSE(accepted(base, "'system', 'draft', " + test::content_sql(respelled))) << "and a time that exists";
    auto with_photo = test::standard("Hello.");
    with_photo.media_id = 424242;
    EXPECT_THROW(insert_local_post(spa_, base, "'system', 'draft', " + test::content_sql(with_photo)), pqxx::foreign_key_violation)
        << "a photo is an upload the table has";
    EXPECT_FALSE(accepted("origin, status, content", "'google', 'sent', " + test::content_sql(test::standard("Hi."))))
        << "a post made at Google has Google's name";
    EXPECT_FALSE(accepted("origin, status, content, google_name",
                          "'google', 'draft', " + test::content_sql(test::standard("Hi.")) + ", 'accounts/1/locations/2/localPosts/3'"))
        << "and is never a draft";
    EXPECT_FALSE(accepted(base + ", removal_requested_at", draft_values + ", '2026-11-19T10:00:00Z'"))
        << "nothing is removed that Google does not have";
}

TEST_F(LocalPosts, OnePostAtGoogleIsOneRow) {
    const std::string columns = "origin, status, content, google_name";
    const std::string values = "'google', 'sent', " + test::content_sql(test::standard("Hi.")) + ", 'accounts/111/locations/9001/localPosts/5'";
    insert_local_post(spa_, columns, values);
    EXPECT_THROW(insert_local_post(spa_, columns, values), pqxx::unique_violation);
}

// ---- the client's line about its customers ------------------------------------------------

TEST_F(LocalPosts, AClientSaysWhoItsCustomersAre) {
    const auto target = "/api/clients/" + std::to_string(spa_client_);
    EXPECT_EQ(read(target).at("audience").as_string(), "") << "nobody has written one";

    const auto changed = patch(target, R"({"audience": "  Office workers nearby, many of them Mandarin speakers.  "})");
    ASSERT_EQ(changed.status, 200u) << changed.body;
    EXPECT_EQ(json(changed).at("audience").as_string(), "Office workers nearby, many of them Mandarin speakers.");
    EXPECT_EQ(json(patch(target, R"({"notes": "x"})")).at("audience").as_string(),
              "Office workers nearby, many of them Mandarin speakers.")
        << "a field that isn't sent stays";

    const auto log = read("/api/audit-log?object_type=client&object_id=" + std::to_string(spa_client_)).at("items").as_array();
    EXPECT_EQ(log[1].at("before").at("audience").as_string(), "");
    EXPECT_EQ(log[1].at("after").at("audience").as_string(), "Office workers nearby, many of them Mandarin speakers.");

    // 500 characters, counted as characters: Chinese is not cut short.
    std::string limit;
    for (int i = 0; i < 500; ++i) limit += "客";
    EXPECT_EQ(patch(target, boost::json::serialize(boost::json::object{{"audience", limit}})).status, 200u);
    EXPECT_EQ(patch(target, boost::json::serialize(boost::json::object{{"audience", limit + "客"}})).status, 400u);
    EXPECT_EQ(patch(target, R"({"audience": 5})").status, 400u);

    const auto created = post("/api/clients", R"({"name": "New Client", "audience": "Families."})");
    ASSERT_EQ(created.status, 201u) << created.body;
    EXPECT_EQ(json(created).at("audience").as_string(), "Families.");
}

// ---- copying a post as a new draft (D-094) ----------------------------------------------------

TEST_F(LocalPosts, ACopyOfAPostWrittenHereIsANewDraftOfTheCallerWithItsUpload) {
    const auto media = insert_upload({});
    auto content = test::standard("Fall specials start Monday.");
    content.call_to_action = domain::LocalPostCallToAction{.action_type = "book", .url = "https://example.com/book"};
    content.media_id = media;
    content.scheduled_time = "2026-12-19T17:00:00.000000Z";
    // Sent long ago by someone else: the copy is the caller's.
    const auto source = insert_local_post(spa_, "origin, status, content, google_name, google_state, sent_at, google_media",
                                          "'system', 'sent', " + test::content_sql(content) +
                                              R"(, 'accounts/111/locations/9001/localPosts/41', 'scheduled', '2026-11-01T00:00:00Z',
                                                 '[{"format": "photo", "url": "https://lh3.googleusercontent.com/p/a"}]')");
    const auto copied = post(std::format("/api/local-posts/{}/copy", source), "");
    ASSERT_EQ(copied.status, 201u) << copied.body;
    const auto copy = json(copied);
    EXPECT_NE(copy.at("id").as_int64(), source);
    EXPECT_EQ(copy.at("status").as_string(), "draft");
    EXPECT_EQ(copy.at("origin").as_string(), "system");
    EXPECT_EQ(copy.at("location_id").as_int64(), spa_);
    EXPECT_EQ(copy.at("created_by").as_string(), "staff@xvisionmediaus.com");
    EXPECT_TRUE(copy.at("status_reason").is_null()) << "nothing was left out";
    EXPECT_TRUE(copy.at("google_name").is_null());
    EXPECT_EQ(copy.at("summary").as_string(), "Fall specials start Monday.");
    EXPECT_EQ(copy.at("call_to_action").at("url").as_string(), "https://example.com/book");
    EXPECT_EQ(copy.at("scheduled_time").as_string(), "2026-12-19T17:00:00.000000Z");
    EXPECT_EQ(copy.at("media_id").as_int64(), media) << "one upload serves several posts";
    ASSERT_EQ(copy.at("media").as_array().size(), 1u);
    EXPECT_EQ(copy.at("media").as_array()[0].at("url").as_string(), "https://storage.googleapis.com/poni-public-test/p/abc123.jpg");
    EXPECT_TRUE(std::ranges::all_of(copy.at("checks").as_array(), [](const auto& check) { return check.at("passed").as_bool(); }));

    EXPECT_EQ(status_of(source), "sent") << "the post copied is untouched";
    EXPECT_EQ(count("jobs"), 0) << "nothing is queued: the copy waits for an approval like any draft";
    const auto log = read(std::format("/api/audit-log?object_type=local_post&object_id={}", copy.at("id").as_int64())).at("items").as_array();
    ASSERT_EQ(log.size(), 1u);
    EXPECT_EQ(log[0].at("action").as_string(), "local_post.copy");
    EXPECT_EQ(log[0].at("actor").as_string(), "staff@xvisionmediaus.com");
    EXPECT_EQ(log[0].at("after").at("copied_from").as_int64(), source);
    EXPECT_EQ(log[0].at("after").at("content").at("summary").as_string(), "Fall specials start Monday.");
}

TEST_F(LocalPosts, ACopyOfAPostMadeAtGoogleBringsNoPhotoOrVideoAndSaysToUploadItAgain) {
    for (const std::string format : {"photo", "video"}) {
        const auto source = insert_local_post(
            buffet_, "origin, status, content, google_name, google_state, google_media",
            "'google', 'sent', " + test::content_sql(test::standard("Made at Google.")) +
                std::format(R"(, 'accounts/111/locations/9002/localPosts/{}', 'live', '[{{"format": "{}", "url": "https://lh3.googleusercontent.com/p/x"}}]')",
                            format == "photo" ? 61 : 62, format));
        const auto copied = post(std::format("/api/local-posts/{}/copy", source), "");
        ASSERT_EQ(copied.status, 201u) << copied.body;
        const auto copy = json(copied);
        EXPECT_EQ(copy.at("location_id").as_int64(), buffet_);
        EXPECT_EQ(copy.at("origin").as_string(), "system");
        EXPECT_TRUE(copy.at("media_id").is_null()) << format;
        EXPECT_TRUE(copy.at("media").as_array().empty()) << "Google's copy of the file is not shown as the draft's";
        EXPECT_THAT(std::string(copy.at("status_reason").as_string()), testing::HasSubstr("upload the " + format + " again"));
    }
    EXPECT_EQ(count("local_post_media"), 0) << "nothing was fetched";
}

TEST_F(LocalPosts, WhatADraftCannotHoldIsLeftOutOfACopyAndNamed) {
    // An alert, with a button only Google writes and a time that has passed.
    auto content = test::standard("Closed for the storm today.");
    content.topic_type = "alert";
    content.call_to_action = domain::LocalPostCallToAction{.action_type = "get_offer", .url = "https://example.com"};
    content.scheduled_time = "2026-01-05T09:00:00.000000Z";
    const auto source = insert_local_post(buffet_, "origin, status, content, google_name, google_state",
                                          "'google', 'sent', " + test::content_sql(content) + ", 'accounts/111/locations/9002/localPosts/63', 'live'");
    const auto copied = post(std::format("/api/local-posts/{}/copy", source), "");
    ASSERT_EQ(copied.status, 201u) << copied.body;
    const auto copy = json(copied);
    EXPECT_EQ(copy.at("topic_type").as_string(), "standard");
    EXPECT_TRUE(copy.at("call_to_action").is_null());
    EXPECT_TRUE(copy.at("scheduled_time").is_null());
    EXPECT_EQ(copy.at("summary").as_string(), "Closed for the storm today.");
    const std::string reason(copy.at("status_reason").as_string());
    EXPECT_THAT(reason, testing::HasSubstr(std::format("Copied from post {}", source)));
    EXPECT_THAT(reason, testing::HasSubstr("\"alert\""));
    EXPECT_THAT(reason, testing::HasSubstr("the button \"get_offer\""));
    EXPECT_THAT(reason, testing::HasSubstr("the time to publish at, which has passed"));
    // The copy is a draft like any other: it can be edited, and its reason goes with the edit.
    const auto saved = put(std::format("/api/local-posts/{}", copy.at("id").as_int64()), kStandard);
    ASSERT_EQ(saved.status, 200u) << saved.body;
    EXPECT_TRUE(json(saved).at("status_reason").is_null());
}

TEST_F(LocalPosts, AnOfferWhoseDatesGoogleLacksIsCopiedAsAnUpdateAndOneNoDraftCanHoldIsRefused) {
    // An offer as Google may list it: no event (its schedule lacked a date), a code, a button.
    auto offer = test::standard("Two for one.");
    offer.topic_type = "offer";
    offer.offer = domain::LocalPostOffer{.coupon_code = "TWO", .redeem_online_url = std::nullopt, .terms_conditions = std::nullopt};
    offer.call_to_action = domain::LocalPostCallToAction{.action_type = "shop", .url = "https://example.com"};
    const auto source = insert_local_post(buffet_, "origin, status, content, google_name, google_state",
                                          "'google', 'sent', " + test::content_sql(offer) + ", 'accounts/111/locations/9002/localPosts/65', 'live'");
    const auto copied = post(std::format("/api/local-posts/{}/copy", source), "");
    ASSERT_EQ(copied.status, 201u) << copied.body;
    const auto copy = json(copied);
    EXPECT_EQ(copy.at("topic_type").as_string(), "standard");
    EXPECT_TRUE(copy.at("offer").is_null());
    EXPECT_EQ(copy.at("call_to_action").at("action_type").as_string(), "shop") << "an update may have the button";
    EXPECT_THAT(std::string(copy.at("status_reason").as_string()), testing::HasSubstr("whose dates Google's post lacks"));

    // A code longer than a draft takes: no draft can hold it, so nothing is written.
    offer.event = domain::LocalPostEvent{.title = "Two for one",
                                         .schedule = {.start_date = "2026-12-01", .start_time = "00:00", .end_date = "2026-12-31", .end_time = "23:59"},
                                         .recurrence = std::nullopt};
    offer.call_to_action.reset();
    offer.offer->coupon_code = std::string(59, 'A');
    const auto long_code = insert_local_post(buffet_, "origin, status, content, google_name, google_state",
                                             "'google', 'sent', " + test::content_sql(offer) + ", 'accounts/111/locations/9002/localPosts/66', 'live'");
    const auto drafts = count("local_posts WHERE status = 'draft'");
    const auto refused = post(std::format("/api/local-posts/{}/copy", long_code), "");
    EXPECT_EQ(refused.status, 409u) << refused.body;
    EXPECT_EQ(error_code(refused), "local_post_not_copyable");
    EXPECT_EQ(count("local_posts WHERE status = 'draft'"), drafts);
}

TEST_F(LocalPosts, AnyPostCanBeCopiedWhateverItsStatus) {
    for (const std::string status : {"draft", "approved", "sending", "sent", "unknown", "failed", "rejected", "removed"}) {
        const auto source = in_status(status, "Post that was " + status + ".");
        const auto copied = post(std::format("/api/local-posts/{}/copy", source), "");
        ASSERT_EQ(copied.status, 201u) << status << ": " << copied.body;
        EXPECT_EQ(json(copied).at("status").as_string(), "draft") << status;
        EXPECT_EQ(json(copied).at("summary").as_string(), "Post that was " + status + ".");
        EXPECT_EQ(status_of(source), status) << "the post copied stays as it was";
    }
    EXPECT_EQ(count("jobs"), 0);
}

TEST_F(LocalPosts, ACopyIsRefusedWhereWritingADraftIsRefused) {
    const auto source = draft(spa_, "Still here.");
    const auto target = std::format("/api/local-posts/{}/copy", source);
    EXPECT_EQ(post("/api/local-posts/999999/copy", "").status, 404u);
    EXPECT_EQ(call(verb::post, target).status, 401u);
    EXPECT_EQ(call(verb::post, target, {.cookie = browser_.cookie, .csrf = ""}).status, 403u) << "a change needs the CSRF token";
    exec(std::format("UPDATE locations SET client_id = NULL WHERE id = {}", spa_));
    const auto unlinked = post(target, "");
    EXPECT_EQ(unlinked.status, 409u);
    EXPECT_EQ(error_code(unlinked), "location_not_synced") << "as writing a draft for that location";
    EXPECT_EQ(count("local_posts"), 1);
}

// ---- deleting a draft (D-094) --------------------------------------------------------------------

TEST_F(LocalPosts, ADraftOrARejectedPostIsDeletedAndTheAuditLogKeepsWhatItSaid) {
    const auto media = insert_upload({});
    auto content = test::standard("Never sent.");
    content.media_id = media;
    const auto a_draft = insert_local_post(spa_, "origin, status, content, created_by",
                                           std::format("'system', 'draft', {}, {}", test::content_sql(content), user_));
    const auto deleted = del(std::format("/api/local-posts/{}", a_draft));
    ASSERT_EQ(deleted.status, 204u) << deleted.body;
    EXPECT_EQ(count(std::format("local_posts WHERE id = {}", a_draft)), 0);
    EXPECT_EQ(get(std::format("/api/local-posts/{}", a_draft), browser_).status, 404u);
    EXPECT_EQ(count(std::format("local_post_media WHERE id = {}", media)), 1) << "the upload stays for other posts";
    const auto log = read(std::format("/api/audit-log?object_type=local_post&object_id={}", a_draft)).at("items").as_array();
    ASSERT_EQ(log.size(), 1u);
    EXPECT_EQ(log[0].at("action").as_string(), "local_post.delete");
    EXPECT_EQ(log[0].at("actor").as_string(), "staff@xvisionmediaus.com");
    const auto& before = log[0].at("before");
    EXPECT_EQ(before.at("status").as_string(), "draft");
    EXPECT_EQ(before.at("location_id").as_int64(), spa_);
    EXPECT_EQ(before.at("created_by_user_id").as_int64(), user_);
    EXPECT_EQ(before.at("content").at("summary").as_string(), "Never sent.");
    EXPECT_EQ(before.at("content").at("media_id").as_int64(), media);
    EXPECT_EQ(del(std::format("/api/local-posts/{}", a_draft)).status, 404u) << "deleting again finds nothing";

    const auto rejected = in_status("rejected", "Decided against.");
    exec(std::format("UPDATE local_posts SET status_reason = 'Wrong week.' WHERE id = {}", rejected));
    ASSERT_EQ(del(std::format("/api/local-posts/{}", rejected)).status, 204u);
    const auto rejected_log = read(std::format("/api/audit-log?object_type=local_post&object_id={}", rejected)).at("items").as_array();
    ASSERT_EQ(rejected_log.size(), 1u);
    EXPECT_EQ(rejected_log[0].at("before").at("status").as_string(), "rejected");
    EXPECT_EQ(rejected_log[0].at("before").at("status_reason").as_string(), "Wrong week.");
    EXPECT_EQ(count("jobs"), 0) << "nothing goes to Google";
}

TEST_F(LocalPosts, NothingGoogleHasOrMayHaveIsDeleted) {
    for (const std::string status : {"approved", "sending", "sent", "unknown", "failed", "removed"}) {
        const auto id = in_status(status);
        const auto refused = del(std::format("/api/local-posts/{}", id));
        EXPECT_EQ(refused.status, 409u) << status;
        EXPECT_EQ(error_code(refused), "local_post_not_open") << status;
        EXPECT_EQ(status_of(id), status) << "the row stays";
    }
    // A post made in Google's own interface.
    const auto at_google = insert_local_post(buffet_, "origin, status, content, google_name, google_state",
                                             "'google', 'sent', " + test::content_sql(test::standard("Made at Google.")) +
                                                 ", 'accounts/111/locations/9002/localPosts/64', 'live'");
    EXPECT_EQ(error_code(del(std::format("/api/local-posts/{}", at_google))), "local_post_not_open");
    EXPECT_EQ(count("audit_log WHERE action = 'local_post.delete'"), 0);

    const auto a_draft = draft(spa_, "Kept.");
    const auto target = std::format("/api/local-posts/{}", a_draft);
    EXPECT_EQ(call(verb::delete_, target).status, 401u);
    EXPECT_EQ(call(verb::delete_, target, {.cookie = browser_.cookie, .csrf = ""}).status, 403u);
    EXPECT_EQ(count(std::format("local_posts WHERE id = {}", a_draft)), 1);
}

// ---- the list by several states (D-094) -------------------------------------------------------------

TEST_F(LocalPosts, TheListFiltersBySeveralStatusesAndGoogleStates) {
    const auto a_draft = in_status("draft");
    const auto rejected = in_status("rejected");
    const auto live = in_status("sent");
    const auto scheduled = in_status("sent");
    exec(std::format("UPDATE local_posts SET google_state = 'scheduled' WHERE id = {}", scheduled));
    const auto recurring = in_status("sent");
    exec(std::format("UPDATE local_posts SET google_state = 'recurring' WHERE id = {}", recurring));
    const auto removed = in_status("removed");
    const auto sorted = [](std::vector<std::int64_t> v) {
        std::ranges::sort(v);
        return v;
    };

    EXPECT_EQ(sorted(ids("?status=draft,rejected")), sorted({a_draft, rejected}));
    EXPECT_EQ(sorted(ids("?status=draft&status=rejected")), sorted({a_draft, rejected})) << "the parameter repeated says the same";
    EXPECT_EQ(sorted(ids("?google_state=live,recurring")), sorted({live, recurring, removed})) << "a removed post keeps the state last read";
    EXPECT_EQ(sorted(ids("?status=sent&google_state=live,recurring")), sorted({live, recurring}));
    EXPECT_EQ(ids("?status=sent&google_state=scheduled"), (std::vector<std::int64_t>{scheduled}));
    EXPECT_EQ(sorted(ids("?status=rejected,removed,failed")), sorted({rejected, removed}));
    EXPECT_EQ(ids("?status=draft"), (std::vector<std::int64_t>{a_draft}));
    EXPECT_EQ(ids("?status=").size(), 6u) << "an empty value is no filter";
    EXPECT_EQ(read("/api/local-posts?status=sent&google_state=live,recurring").at("total").as_int64(), 2);

    EXPECT_EQ(sorted(ids("?google_state=live,")), sorted({live, removed})) << "a trailing comma adds an empty value, which is none";
    for (const auto* bad : {"?status=draft,posted", "?google_state=gone", "?status=Draft"}) {
        EXPECT_EQ(get(std::string("/api/local-posts") + bad, browser_).status, 400u) << bad;
    }
}

// ---- the phone app's second round (D-098) ---------------------------------------------------------

TEST_F(LocalPosts, TheListIsOldestFirstWithOrderAscAndPagesTheSameWayBack) {
    const auto old_google = insert_local_post(buffet_, "origin, status, content, google_name, google_state, google_created_at",
                                              "'google', 'sent', " + test::content_sql(test::standard("Summer night.")) +
                                                  ", 'accounts/111/locations/9002/localPosts/1', 'live', '2026-08-28T22:40:31Z'");
    // Two drafts written at the same moment: the id decides between them, in both directions.
    const auto first = draft(spa_, "First.");
    const auto same_time = draft(buffet_, "Same time.");
    clock_.advance(1min);
    const auto last = draft(spa_, "Last.");

    const std::vector<std::int64_t> newest_first{last, same_time, first, old_google};
    EXPECT_EQ(ids(""), newest_first) << "without `order`, as before";
    EXPECT_EQ(ids("?order=desc"), newest_first);
    const std::vector<std::int64_t> oldest_first(newest_first.rbegin(), newest_first.rend());
    EXPECT_EQ(ids("?order=asc"), oldest_first);

    // Page by page, each direction is the other one read backwards.
    std::vector<std::int64_t> paged;
    for (const auto* page : {"1", "2"}) {
        const auto items = ids(std::format("?order=asc&per_page=2&page={}", page));
        EXPECT_EQ(items.size(), 2u) << page;
        paged.insert(paged.end(), items.begin(), items.end());
    }
    EXPECT_EQ(paged, oldest_first);
    EXPECT_EQ(read("/api/local-posts?order=asc&per_page=2&page=2").at("total").as_int64(), 4);
    EXPECT_EQ(ids("?order=asc&status=draft&location_id=" + std::to_string(spa_)), (std::vector<std::int64_t>{first, last}));

    for (const auto* bad : {"?order=up", "?order=ASC", "?order=asc,desc"}) {
        EXPECT_EQ(get(std::string("/api/local-posts") + bad, browser_).status, 400u) << bad;
    }
    EXPECT_EQ(ids("?order="), newest_first) << "an empty value is no value, as for reviews";
}

TEST_F(LocalPosts, GoogleStateNoneTakesThePostsThatHaveNoState) {
    const auto a_draft = in_status("draft");
    const auto rejected = in_status("rejected");
    // Sent, and not yet read back from Google.
    const auto not_read_back = in_status("sent");
    exec(std::format("UPDATE local_posts SET google_state = NULL WHERE id = {}", not_read_back));
    const auto live = in_status("sent");
    const auto processing = in_status("sent");
    exec(std::format("UPDATE local_posts SET google_state = 'processing' WHERE id = {}", processing));
    const auto sorted = [](std::vector<std::int64_t> v) {
        std::ranges::sort(v);
        return v;
    };

    EXPECT_EQ(sorted(ids("?google_state=none")), sorted({a_draft, rejected, not_read_back}));
    EXPECT_EQ(sorted(ids("?google_state=none,live")), sorted({a_draft, rejected, not_read_back, live})) << "both kinds";
    EXPECT_EQ(sorted(ids("?google_state=live&google_state=none")), sorted({a_draft, rejected, not_read_back, live}));
    EXPECT_EQ(ids("?google_state=none&status=sent"), (std::vector<std::int64_t>{not_read_back}));
    EXPECT_EQ(sorted(ids("?google_state=none,live&status=sent")), sorted({not_read_back, live}));
    EXPECT_EQ(sorted(ids("?google_state=none,processing&status=draft,sent")), sorted({a_draft, not_read_back, processing}));
    EXPECT_EQ(read("/api/local-posts?google_state=none").at("total").as_int64(), 3);
    EXPECT_EQ(sorted(ids("?google_state=live,processing")), sorted({live, processing})) << "without `none`, as before";

    // `none` is a word of the filter: the posts it finds have no state.
    for (const auto& item : read("/api/local-posts?google_state=none").at("items").as_array()) {
        EXPECT_TRUE(item.at("google_state").is_null()) << item.at("id").as_int64();
    }
    for (const auto* bad : {"?google_state=None", "?google_state=null", "?status=none"}) {
        EXPECT_EQ(get(std::string("/api/local-posts") + bad, browser_).status, 400u) << bad;
    }
}

TEST_F(LocalPosts, ACopyNamesItsSourceAndTheSourceListsItsCopiesNewestFirst) {
    const auto source = draft(spa_, "Fall specials start Monday.");
    const auto copy_of = [&](std::int64_t id) {
        const auto copied = post(std::format("/api/local-posts/{}/copy", id), "");
        EXPECT_EQ(copied.status, 201u) << copied.body;
        return json(copied);
    };
    const auto first = copy_of(source);
    EXPECT_EQ(first.at("copied_from_id").as_int64(), source) << "the copy's answer says what it was copied from";
    EXPECT_TRUE(first.at("copies").as_array().empty());
    const auto first_id = first.at("id").as_int64();
    clock_.advance(1min);
    const auto second_id = copy_of(source).at("id").as_int64();

    const auto copies = [&](std::int64_t id) {
        std::vector<std::pair<std::int64_t, std::string>> out;
        for (const auto& c : read(std::format("/api/local-posts/{}", id)).at("copies").as_array()) {
            out.emplace_back(c.at("id").as_int64(), std::string(c.at("status").as_string()));
            EXPECT_EQ(c.as_object().size(), 2u) << "an id and a status";
        }
        return out;
    };
    using Copies = std::vector<std::pair<std::int64_t, std::string>>;
    EXPECT_EQ(copies(source), (Copies{{second_id, "draft"}, {first_id, "draft"}}));
    EXPECT_TRUE(read(std::format("/api/local-posts/{}", source)).at("copied_from_id").is_null()) << "the source was written new";
    EXPECT_EQ(read(std::format("/api/local-posts/{}", second_id)).at("copied_from_id").as_int64(), source);
    // The list shows the same.
    for (const auto& item : read("/api/local-posts").at("items").as_array()) {
        if (item.at("id").as_int64() == source) EXPECT_EQ(item.at("copies").as_array().size(), 2u);
    }

    // The status in `copies` is the copy's as it is now; the source is untouched by what happens to a copy.
    ASSERT_EQ(post(std::format("/api/local-posts/{}/reject", first_id), "").status, 200u);
    EXPECT_EQ(copies(source), (Copies{{second_id, "draft"}, {first_id, "rejected"}}));
    EXPECT_EQ(status_of(source), "draft");

    // A copy that is deleted leaves its source's list.
    ASSERT_EQ(del(std::format("/api/local-posts/{}", second_id)).status, 204u);
    EXPECT_EQ(copies(source), (Copies{{first_id, "rejected"}}));
    ASSERT_EQ(del(std::format("/api/local-posts/{}", first_id)).status, 204u);
    EXPECT_TRUE(copies(source).empty());

    // A post that was copied may be deleted while it is a draft; its copy keeps its content and loses the link.
    const auto copy = copy_of(source).at("id").as_int64();
    ASSERT_EQ(del(std::format("/api/local-posts/{}", source)).status, 204u);
    const auto orphan = read(std::format("/api/local-posts/{}", copy));
    EXPECT_TRUE(orphan.at("copied_from_id").is_null());
    EXPECT_EQ(orphan.at("summary").as_string(), "Fall specials start Monday.");
    EXPECT_EQ(orphan.at("status").as_string(), "draft");
    EXPECT_EQ(count("jobs"), 0) << "nothing about the link goes to Google";
}

TEST_F(LocalPosts, ACopyOfACopyNamesTheCopyAndAPostAtGoogleListsItsCopies) {
    const auto at_google = insert_local_post(buffet_, "origin, status, content, google_name, google_state",
                                             "'google', 'sent', " + test::content_sql(test::standard("Made at Google.")) +
                                                 ", 'accounts/111/locations/9002/localPosts/71', 'live'");
    const auto copy = json(post(std::format("/api/local-posts/{}/copy", at_google), "")).at("id").as_int64();
    const auto copy_of_copy = json(post(std::format("/api/local-posts/{}/copy", copy), ""));
    EXPECT_EQ(copy_of_copy.at("copied_from_id").as_int64(), copy) << "the post copied, not the first of the line";
    const auto source = read(std::format("/api/local-posts/{}", at_google));
    ASSERT_EQ(source.at("copies").as_array().size(), 1u);
    EXPECT_EQ(source.at("copies").as_array()[0].at("id").as_int64(), copy);
    EXPECT_EQ(source.at("status").as_string(), "sent");
    EXPECT_EQ(source.at("google_state").as_string(), "live") << "copying changes nothing about the post at Google";
}

TEST_F(LocalPosts, APostTheSyncFoundAndADraftWrittenNewHaveNoLinks) {
    const auto made = test::make_local_post("111", "9002", "777", "Now open late on Fridays.");
    await(db().tx([&](pqxx::work& tx) { repo::store_local_posts(tx, buffet_, {made}, clock_.now()); }));
    const auto found = value<std::int64_t>("SELECT id FROM local_posts WHERE google_name = 'accounts/111/locations/9002/localPosts/777'");
    const auto written = post(std::format("/api/locations/{}/local-posts", spa_), kStandard);
    ASSERT_EQ(written.status, 201u) << written.body;
    EXPECT_TRUE(json(written).at("copied_from_id").is_null());
    EXPECT_TRUE(json(written).at("copies").as_array().empty());
    for (const auto id : {found, json(written).at("id").as_int64()}) {
        const auto shown = read(std::format("/api/local-posts/{}", id));
        EXPECT_TRUE(shown.at("copied_from_id").is_null()) << id;
        EXPECT_TRUE(shown.at("copies").is_array()) << id;
        EXPECT_TRUE(shown.at("copies").as_array().empty()) << id;
    }
}
