// Drafts of local posts, their approval and the send, over a real PostgreSQL
// and a fake Google that keeps a new post in processing (D-088 points 3, 5,
// 6 and 10): every state a post passes through, and every way a send must
// not happen, or must not happen twice.

#include "local_post_fixture.hpp"

#include <gmock/gmock.h>


using namespace poni;
using namespace std::chrono_literals;
using test::kBuffetPosts;
using test::kSpaPosts;
using test::standard;
using testing::HasSubstr;
using Outcome = repo::LocalPostSendClaim::Outcome;

namespace {

class LocalPostsPublish : public test::LocalPostTest {
protected:
    void SetUp() override {
        LocalPostTest::SetUp();
        // Off after installation; most tests are about what happens when a person has switched it on.
        exec("UPDATE system_settings SET posting_enabled = true");
    }

    repo::Result<model::LocalPost> create(std::int64_t location, const domain::LocalPostContent& content) {
        return as_staff([&](pqxx::work& tx, audit::Writer& audit) { return repo::create_local_post(tx, audit, location, content, test::kMediaBase); });
    }
    repo::Result<model::LocalPost> edit(std::int64_t id, const domain::LocalPostContent& content) {
        return as_staff([&](pqxx::work& tx, audit::Writer& audit) { return repo::edit_local_post(tx, audit, id, content, test::kMediaBase); });
    }
    repo::Result<model::LocalPost> reject(std::int64_t id, const std::string& reason = {}) {
        return as_staff([&](pqxx::work& tx, audit::Writer& audit) { return repo::reject_local_post(tx, audit, id, reason, test::kMediaBase); });
    }
    repo::Result<model::LocalPost> approve(std::int64_t id, const domain::LocalPostContent& content) {
        return as_staff([&](pqxx::work& tx, audit::Writer& audit) {
            return repo::approve_local_post(tx, audit, id, content, test::kMediaBase);
        });
    }

    // The content as the API would show it to the person who approves.
    domain::LocalPostContent shown(std::int64_t id) {
        pqxx::work tx{conn()};
        return repo::get_local_post(tx, id, test::kMediaBase)->content();
    }
    // A worker's claim of the send, committed: what the job does before it calls Google.
    Outcome claim(std::int64_t id) {
        return tx([&](pqxx::work& tx) { return repo::claim_local_post_send(tx, id, clock_.now(), test::kMediaBase).outcome; });
    }
    // Approves what is stored, as a person who has read it does.
    repo::Result<model::LocalPost> approve(std::int64_t id) { return approve(id, shown(id)); }

    std::int64_t draft(std::int64_t location, const domain::LocalPostContent& content = standard()) {
        const auto created = create(location, content);
        EXPECT_TRUE(created) << (created ? "" : created.error().message);
        return created ? created->id : 0;
    }

    void approve_and_send(std::int64_t id) {
        const auto approved = approve(id);
        ASSERT_TRUE(approved) << approved.error().code << ": " << approved.error().message;
        run_jobs();
    }

    void publish(std::int64_t id, bool last_attempt = false) { await(local_posts_->publish(id, last_attempt)); }

    std::string failed_checks(std::int64_t id) {
        return value<std::string>(std::format("SELECT coalesce(string_agg(c->>'name', ',' ORDER BY c->>'name'), '') FROM local_posts p, "
                                              "jsonb_array_elements(p.checks) c WHERE p.id = {} AND NOT (c->>'passed')::boolean",
                                              id));
    }

    std::int64_t upload(const std::string& object = "p/abc123.jpg", const std::string& age = "0 days") {
        return insert_upload({.object = object, .age = age});
    }
};

}  // namespace

// ---- the happy path, state by state ------------------------------------------------------

TEST_F(LocalPostsPublish, AnApprovedPostIsSentOnceWithItsTimeAndRepeatAndGoogleSchedulesIt) {
    auto content = standard("Live music every Friday evening.");
    content.topic_type = "event";
    content.event = domain::LocalPostEvent{.title = "Friday live music",
                                           .schedule = {"2026-12-19", "18:00", "2026-12-19", "20:00"},
                                           .recurrence = domain::LocalPostRecurrence{"weekly", "2027-01-31T00:00:00Z"}};
    content.scheduled_time = "2026-12-19T17:00:00Z";
    const auto id = draft(spa_, content);
    EXPECT_EQ(post(id, "status"), "draft");
    EXPECT_EQ(failed_checks(id), "");
    EXPECT_TRUE(google_.creates.empty()) << "a draft goes nowhere";

    const auto approved = approve(id);
    ASSERT_TRUE(approved) << approved.error().message;
    EXPECT_EQ(approved->status, "approved");
    EXPECT_EQ(approved->decided_by, "staff@xvisionmediaus.com");
    EXPECT_TRUE(google_.creates.empty()) << "the approval itself sends nothing";
    EXPECT_EQ(count("jobs WHERE kind = 'local_posts.publish'"), 1);

    run_jobs();
    ASSERT_EQ(google_.creates.size(), 1u);
    EXPECT_EQ(google_.creates[0].location_id, kSpaPosts);
    EXPECT_EQ(google_.creates[0].body, gbp::create_body(shown(id), {})) << "the content that was approved, field for field";
    EXPECT_EQ(google_.creates[0].body.at("scheduledTime").as_string(), "2026-12-19T17:00:00.000000Z");
    EXPECT_TRUE(google_.creates[0].body.at("event").at("recurrenceInfo").as_object().contains("weeklyPattern"));
    EXPECT_EQ(post(id, "status"), "sent");
    EXPECT_EQ(post(id, "google_name"), name_of(kSpaPosts, 7001));
    EXPECT_EQ(post(id, "google_state"), "processing") << "Google has it and is taking it in";
    EXPECT_NE(post(id, "sent_at"), "");
    EXPECT_NE(post(id, "send_started_at"), "");
    EXPECT_EQ(post(id, "origin"), "system");

    // Google is read again 1, 5 and 15 minutes after the send; the engine has no timer of its own for the post.
    EXPECT_EQ(count("jobs WHERE kind = 'local_posts.sync' AND status = 'queued'"), 3);
    google_.finish_processing();
    clock_.advance(1min);
    run_jobs();
    EXPECT_EQ(post(id, "google_state"), "scheduled");
    EXPECT_EQ(google_.creates.size(), 1u);
    EXPECT_EQ(count("local_posts"), 1);

    // Who did what: the person approved, the engine sent.
    EXPECT_EQ(value<std::string>("SELECT actor FROM audit_log WHERE action = 'local_post.approve'"), "staff@xvisionmediaus.com");
    EXPECT_EQ(value<std::string>("SELECT after->'content'->>'summary' FROM audit_log WHERE action = 'local_post.approve'"),
              "Live music every Friday evening.");
    EXPECT_EQ(value<std::string>("SELECT actor FROM audit_log WHERE action = 'local_post.sent'"), "system:local-posts");
    EXPECT_EQ(audited("local_post.send_started"), 1);
    EXPECT_EQ(problems(), 0);
}

TEST_F(LocalPostsPublish, APostsPhotoIsGivenToGoogleByItsPublicAddress) {
    auto content = standard("A look at the new room.");
    content.media_ids = {upload()};
    const auto id = draft(spa_, content);
    EXPECT_EQ(failed_checks(id), "");
    approve_and_send(id);
    ASSERT_EQ(google_.creates.size(), 1u);
    EXPECT_EQ(google_.creates[0].body.at("media"),
              boost::json::parse(R"([{"mediaFormat": "PHOTO", "sourceUrl": "https://storage.googleapis.com/poni-public-test/p/abc123.jpg"}])"));
    EXPECT_THAT(post(id, "google_media->0->>'url'"), HasSubstr("googleusercontent.com")) << "from then on the post shows Google's copy";
    EXPECT_EQ(post(id, "(content->'media_ids')"), std::format("[{}]", content.media_ids.front())) << "which upload it was stays on the row";
}

TEST_F(LocalPostsPublish, APostsVideoIsGivenToGoogleAsAVideo) {
    auto content = standard("A walk through the new room.");
    content.media_ids = {insert_upload({.object = "p/walk.mp4", .format = "video", .content_type = "video/mp4", .bytes = 9000000,
                                      .width = 1080, .height = 1920, .duration_ms = "12500"})};
    const auto id = draft(spa_, content);
    EXPECT_EQ(failed_checks(id), "");
    approve_and_send(id);
    ASSERT_EQ(google_.creates.size(), 1u);
    EXPECT_EQ(google_.creates[0].body.at("media"),
              boost::json::parse(R"([{"mediaFormat": "VIDEO", "sourceUrl": "https://storage.googleapis.com/poni-public-test/p/walk.mp4"}])"));
    EXPECT_EQ(post(id, "status"), "sent");
}

TEST_F(LocalPostsPublish, SeveralPhotosAndAVideoAreGivenToGoogleInTheirOrder) {
    // Up to ten files, photos and videos mixed, in the order the person put them (D-101).
    auto content = standard("The new rooms, and a walk through them.");
    const auto walk = insert_upload({.object = "p/walk.mp4", .format = "video", .content_type = "video/mp4", .bytes = 9000000,
                                     .width = 1080, .height = 1920, .duration_ms = "12500"});
    content.media_ids = {upload("p/b.jpg"), walk, upload("p/a.jpg")};
    const auto id = draft(spa_, content);
    EXPECT_EQ(failed_checks(id), "");
    approve_and_send(id);
    ASSERT_EQ(google_.creates.size(), 1u);
    EXPECT_EQ(google_.creates[0].body.at("media"), boost::json::parse(R"([
        {"mediaFormat": "PHOTO", "sourceUrl": "https://storage.googleapis.com/poni-public-test/p/b.jpg"},
        {"mediaFormat": "VIDEO", "sourceUrl": "https://storage.googleapis.com/poni-public-test/p/walk.mp4"},
        {"mediaFormat": "PHOTO", "sourceUrl": "https://storage.googleapis.com/poni-public-test/p/a.jpg"}])"));
    EXPECT_EQ(post(id, "(content->'media_ids')"), std::format("[{}, {}, {}]", content.media_ids[0], content.media_ids[1], content.media_ids[2]));
}

TEST_F(LocalPostsPublish, OneFileGoogleCouldNotFetchBlocksTheApprovalAndIsNamed) {
    auto content = standard("Three rooms.");
    content.media_ids = {upload("p/a.jpg"), upload("p/old.jpg", "361 days"), upload("p/c.jpg")};
    const auto id = draft(spa_, content);
    EXPECT_EQ(failed_checks(id), "media_usable");
    EXPECT_THAT(post(id, "(SELECT c->>'detail' FROM jsonb_array_elements(checks) c WHERE c->>'name' = 'media_usable')"),
                HasSubstr("file 2 of 3 (the photo)"));
    EXPECT_EQ(approve(id).error().code, "checks_failed");
    EXPECT_TRUE(google_.creates.empty());
}

// ---- never twice -----------------------------------------------------------------------------

TEST_F(LocalPostsPublish, RunningThePublishJobAgainSendsNothing) {
    const auto id = draft(spa_);
    approve_and_send(id);
    ASSERT_EQ(google_.creates.size(), 1u);
    publish(id);
    publish(id, true);
    EXPECT_EQ(google_.creates.size(), 1u);
    EXPECT_EQ(post(id, "status"), "sent");
}

TEST_F(LocalPostsPublish, APostNobodyApprovedIsNeverSent) {
    const auto id = draft(spa_);
    publish(id);
    EXPECT_TRUE(google_.creates.empty());
    EXPECT_EQ(post(id, "status"), "draft");

    // The table itself refuses an approval without a person and the content approved.
    EXPECT_THROW(exec(std::format("UPDATE local_posts SET status = 'approved' WHERE id = {}", id)), pqxx::check_violation);

    // Content that is not the content approved is not sent, whoever changed it and however.
    ASSERT_TRUE(approve(id));
    exec(std::format("UPDATE local_posts SET content = jsonb_set(content, '{{summary}}', '\"Something nobody approved.\"') WHERE id = {}", id));
    run_jobs();
    EXPECT_TRUE(google_.creates.empty());
    EXPECT_EQ(post(id, "status"), "draft");
    EXPECT_THAT(post(id, "status_reason"), HasSubstr("not the content that was approved"));

    // A rejected draft stays rejected.
    const auto other = draft(spa_, standard("Not this one."));
    ASSERT_TRUE(reject(other, "Wrong week."));
    publish(other);
    EXPECT_TRUE(google_.creates.empty());
    EXPECT_EQ(post(other, "status"), "rejected");
    EXPECT_EQ(post(other, "status_reason"), "Wrong week.");
    EXPECT_EQ(approve(other).error().code, "local_post_not_open");
}

TEST_F(LocalPostsPublish, APostCannotBeApprovedTwice) {
    const auto id = draft(spa_);
    const auto content = shown(id);
    ASSERT_TRUE(approve(id, content));
    const auto again = approve(id, content);
    ASSERT_FALSE(again);
    EXPECT_EQ(again.error().code, "local_post_not_open");
    EXPECT_EQ(count("jobs WHERE kind = 'local_posts.publish'"), 1) << "one job per approval";
    run_jobs();
    EXPECT_EQ(approve(id, content).error().code, "local_post_not_open") << "nor once it is at Google";
    EXPECT_EQ(google_.creates.size(), 1u);
}

TEST_F(LocalPostsPublish, TwoWorkersThatReachTheClaimTogetherSendOnce) {
    const auto id = draft(spa_);
    ASSERT_TRUE(approve(id));
    // The first worker has claimed and not yet committed; the second arrives and waits for the first worker's lock.
    const auto claim = [&](pqxx::work& tx) { return repo::claim_local_post_send(tx, id, clock_.now(), test::kMediaBase); };
    const auto [mine, theirs] = race(claim, claim);
    ASSERT_EQ(mine.outcome, Outcome::claimed);
    EXPECT_EQ(theirs.outcome, Outcome::not_approved);
    EXPECT_THAT(theirs.detail, HasSubstr("sending"));
    EXPECT_TRUE(theirs.body.empty()) << "nothing to send";
}

// ---- the kill switches (D-088 point 5) -----------------------------------------------------

TEST_F(LocalPostsPublish, WithTheGlobalSwitchOffNothingCanBeApproved) {
    exec("UPDATE system_settings SET posting_enabled = false");
    const auto id = draft(spa_);
    const auto refused = approve(id);
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, "posting_disabled");
    EXPECT_EQ(post(id, "status"), "draft") << "a draft can still be written and edited";
    EXPECT_EQ(count("jobs WHERE kind = 'local_posts.publish'"), 0);
}

TEST_F(LocalPostsPublish, TheSwitchIsReadAgainInTheClaimAndAPostNeverGoesOutLaterByItself) {
    const auto id = draft(spa_);
    ASSERT_TRUE(approve(id));
    exec("UPDATE system_settings SET posting_enabled = false");
    run_jobs();
    EXPECT_TRUE(google_.creates.empty());
    EXPECT_EQ(post(id, "status"), "draft");
    EXPECT_THAT(post(id, "status_reason"), HasSubstr("Posting is switched off"));
    EXPECT_EQ(audited("local_post.send_cancelled"), 1);

    // Switching posting back on sends nothing: a person approves again.
    exec("UPDATE system_settings SET posting_enabled = true");
    clock_.advance(1h);
    await(local_posts_->enqueue_due());
    run_jobs();
    publish(id);
    EXPECT_TRUE(google_.creates.empty());
    approve_and_send(id);
    EXPECT_EQ(google_.creates.size(), 1u);
    EXPECT_EQ(post(id, "status_reason"), "");
}

TEST_F(LocalPostsPublish, ALocationsOwnSwitchStopsThatLocationOnly) {
    exec(std::format("UPDATE locations SET posting_enabled = false WHERE id = {}", buffet_));
    const auto stopped = draft(buffet_);
    const auto refused = approve(stopped);
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, "posting_disabled");
    EXPECT_THAT(refused.error().message, HasSubstr("for this location"));

    approve_and_send(draft(spa_));
    ASSERT_EQ(google_.creates.size(), 1u);
    EXPECT_EQ(google_.creates[0].location_id, kSpaPosts);

    // Flipped between the approval and the send.
    exec(std::format("UPDATE locations SET posting_enabled = true WHERE id = {}", buffet_));
    ASSERT_TRUE(approve(stopped));
    exec(std::format("UPDATE locations SET posting_enabled = false WHERE id = {}", buffet_));
    run_jobs();
    EXPECT_EQ(google_.creates.size(), 1u);
    EXPECT_EQ(post(stopped, "status"), "draft");
}

TEST_F(LocalPostsPublish, NothingIsWrittenOrSentForALocationThatIsSwitchedOff) {
    const auto id = draft(buffet_);
    switch_location(buffet_, false);
    EXPECT_EQ(approve(id).error().code, "location_not_synced");
    EXPECT_EQ(create(buffet_, standard()).error().code, "location_not_synced");
    EXPECT_EQ(create(999999, standard()).error().kind, repo::Failure::Kind::not_found);

    // Switched off between the approval and the send: the claim reads the switch (D-102).
    switch_location(buffet_, true);
    ASSERT_TRUE(approve(id));
    switch_location(buffet_, false);
    run_jobs();
    EXPECT_TRUE(google_.creates.empty());
    EXPECT_EQ(post(id, "status"), "draft");
    EXPECT_THAT(post(id, "status_reason"), HasSubstr("Nothing was sent"));

    // Without a client it is sent like any other.
    exec(std::format("UPDATE locations SET client_id = NULL WHERE id = {}", buffet_));
    switch_location(buffet_, true);
    clock_.advance(1min);
    ASSERT_TRUE(approve(id));
    run_jobs();
    EXPECT_EQ(google_.creates.size(), 1u);
}

// ---- what a person may approve (D-088 points 3 and 10) ---------------------------------------

TEST_F(LocalPostsPublish, ApprovalIsForTheExactContentThePersonRead) {
    auto content = standard("Holiday hours: open until nine.");
    content.call_to_action = domain::LocalPostCallToAction{.action_type = "learn_more", .url = "https://example.com/hours"};
    content.scheduled_time = "2026-12-19T17:00:00Z";
    const auto id = draft(spa_, content);
    const auto read = shown(id);

    // Someone edits the draft after it was shown.
    auto edited = read;
    edited.summary = "Holiday hours: open until ten.";
    ASSERT_TRUE(edit(id, edited));
    const auto stale = approve(id, read);
    ASSERT_FALSE(stale);
    EXPECT_EQ(stale.error().code, "local_post_changed") << "nobody approves an edit they have not seen";

    // Every field counts, not only the text.
    for (const auto& change : std::vector<std::function<void(domain::LocalPostContent&)>>{
             [](auto& c) { c.scheduled_time = "2026-12-20T17:00:00Z"; },
             [](auto& c) { c.scheduled_time.reset(); },
             [](auto& c) { c.call_to_action->url = "https://example.com/other"; },
             [](auto& c) { c.call_to_action.reset(); },
             [](auto& c) { c.language_code = "es"; },
             [&](auto& c) { c.media_ids = {upload("p/other.jpg")}; },
         }) {
        auto other = edited;
        change(other);
        const auto refused = approve(id, other);
        ASSERT_FALSE(refused);
        EXPECT_EQ(refused.error().code, "local_post_changed");
    }
    EXPECT_EQ(post(id, "status"), "draft");
    EXPECT_TRUE(google_.creates.empty());

    // The same content written another way is the same content: surrounding space, and a time with fewer digits.
    auto same = edited;
    same.summary = "  Holiday hours: open until ten.\n";
    same.scheduled_time = "2026-12-19T17:00:00.000Z";
    ASSERT_TRUE(approve(id, same));
    run_jobs();
    ASSERT_EQ(google_.creates.size(), 1u);
    EXPECT_EQ(google_.creates[0].body.at("summary").as_string(), "Holiday hours: open until ten.");
}

TEST_F(LocalPostsPublish, AFailedCheckIsStoredWithTheDraftAndBlocksItsApproval) {
    // What Google removes posts for, and what it refuses.
    const auto with_phone = draft(spa_, standard("Call us on (212) 555-0100 to book."));
    EXPECT_EQ(failed_checks(with_phone), "no_phone");
    const auto refused = approve(with_phone);
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, "checks_failed");
    EXPECT_THAT(refused.error().message, HasSubstr("no_phone"));

    EXPECT_EQ(failed_checks(draft(spa_, standard("Call (212) 555\u20130100 to book."))), "no_phone") << "an en dash between the groups";
    EXPECT_EQ(failed_checks(draft(spa_, standard("Call 212\u00a0555\u00a00100 to book."))), "no_phone") << "non-breaking spaces";
    EXPECT_EQ(failed_checks(draft(spa_, standard("Call 212/555/0100 to book."))), "no_phone");
    EXPECT_EQ(failed_checks(draft(spa_, standard("Write to hello@example.com for a table."))), "no_email");
    EXPECT_EQ(failed_checks(draft(spa_, standard("Follow @ludreamspa for more."))), "no_handle");
    EXPECT_EQ(failed_checks(draft(spa_, standard(""))), "summary_length");
    EXPECT_EQ(failed_checks(draft(spa_, standard(std::string(1501, 'a')))), "summary_length");
    EXPECT_EQ(failed_checks(draft(spa_, standard(std::string(1500, 'a')))), "");
    std::string chinese;
    for (int i = 0; i < 1500; ++i) chinese += "好";
    EXPECT_EQ(failed_checks(draft(spa_, standard(chinese))), "") << "1500 characters, not bytes";

    auto plain_link = standard();
    plain_link.call_to_action = domain::LocalPostCallToAction{.action_type = "book", .url = "http://example.com/book"};
    EXPECT_EQ(failed_checks(draft(spa_, plain_link)), "button_url");
    for (const auto* address : {"https://", "https:///book", "https://example.com/a b", "javascript:alert(1)", "https://example.com/\u202egnp.exe",
                                "https://\u0435xample.com/"}) {
        plain_link.call_to_action->url = address;
        EXPECT_EQ(failed_checks(draft(spa_, plain_link)), "button_url") << address;
    }
    plain_link.call_to_action->url.reset();
    EXPECT_EQ(failed_checks(draft(spa_, plain_link)), "button_url") << "a button other than call needs an address";
    plain_link.call_to_action = domain::LocalPostCallToAction{.action_type = "call", .url = std::nullopt};
    EXPECT_EQ(failed_checks(draft(spa_, plain_link)), "");

    auto past = standard();
    past.scheduled_time = "2026-11-10T11:59:00Z";  // a minute ago
    EXPECT_EQ(failed_checks(draft(spa_, past)), "scheduled_in_future");

    auto event = standard("Open house.");
    event.topic_type = "event";
    event.event = domain::LocalPostEvent{.title = std::string(59, 'T'), .schedule = {"2026-12-20", "10:00", "2026-12-19", "16:00"}, .recurrence = {}};
    EXPECT_EQ(failed_checks(draft(spa_, event)), "event_dates,title_length");
    event.event = domain::LocalPostEvent{.title = "Open house",
                                         .schedule = {"2026-12-19", "10:00", "2026-12-19", "16:00"},
                                         .recurrence = domain::LocalPostRecurrence{"weekly", std::nullopt}};
    EXPECT_EQ(failed_checks(draft(spa_, event)), "recurrence_end") << "a repeat needs an end";
    event.event->recurrence->until = "2026-12-01T00:00:00Z";
    EXPECT_EQ(failed_checks(draft(spa_, event)), "recurrence_end") << "which is not before the event";

    auto offer = standard("Ten percent off.");
    offer.topic_type = "offer";
    offer.event = domain::LocalPostEvent{.title = "Lunch week", .schedule = {"2026-12-19", "11:00", "2026-12-26", "15:30"}, .recurrence = {}};
    offer.offer = domain::LocalPostOffer{.coupon_code = "LUNCH10", .redeem_online_url = "ftp://example.com/x", .terms_conditions = {}};
    EXPECT_EQ(failed_checks(draft(spa_, offer)), "offer_url");

    auto no_language = standard();
    no_language.language_code = "";
    EXPECT_EQ(failed_checks(draft(spa_, no_language)), "language");

    // The draft is fixed, checked again, and can then be approved.
    ASSERT_TRUE(edit(with_phone, standard("Use the button to call us and book.")));
    EXPECT_EQ(failed_checks(with_phone), "");
    EXPECT_EQ(post(with_phone, "edited_by"), std::to_string(user_));
    EXPECT_TRUE(approve(with_phone));
    EXPECT_TRUE(google_.creates.empty()) << "none of the drafts above went anywhere";
}

TEST_F(LocalPostsPublish, WhatIsNotAPostAtAllIsRefusedOutright) {
    const auto refuses = [&](const std::function<void(domain::LocalPostContent&)>& change) {
        auto content = standard();
        change(content);
        const auto result = create(spa_, content);
        return !result && result.error().kind == repo::Failure::Kind::invalid;
    };
    const domain::LocalPostEvent event{.title = "Open house", .schedule = {"2026-12-19", "10:00", "2026-12-19", "16:00"}, .recurrence = {}};
    EXPECT_TRUE(refuses([](auto& c) { c.topic_type = "alert"; })) << "Google's COVID alerts are not written here";
    EXPECT_TRUE(refuses([](auto& c) { c.topic_type = "product"; }));
    EXPECT_TRUE(refuses([&](auto& c) { c.event = event; })) << "a standard post has no event";
    EXPECT_TRUE(refuses([](auto& c) { c.offer = domain::LocalPostOffer{}; }));
    EXPECT_TRUE(refuses([](auto& c) { c.topic_type = "event"; })) << "an event needs its event";
    EXPECT_TRUE(refuses([&](auto& c) { c.topic_type = "event"; c.event = event; c.offer = domain::LocalPostOffer{}; }));
    EXPECT_TRUE(refuses([&](auto& c) {
        c.topic_type = "offer";
        c.event = event;
        c.call_to_action = domain::LocalPostCallToAction{.action_type = "shop", .url = "https://example.com"};
    })) << "an offer has no button";
    EXPECT_TRUE(refuses([](auto& c) { c.call_to_action = domain::LocalPostCallToAction{.action_type = "get_offer", .url = {}}; }));
    EXPECT_TRUE(refuses([](auto& c) { c.call_to_action = domain::LocalPostCallToAction{.action_type = "call", .url = "https://example.com"}; }));
    for (const auto* day : {"2026-02-30", "2026-13-01", "19-12-2026", "tomorrow", ""}) {
        EXPECT_TRUE(refuses([&](auto& c) { c.topic_type = "event"; c.event = event; c.event->schedule.start_date = day; })) << day;
    }
    for (const auto* time : {"24:00", "9:00", "10:60", "noon"}) {
        EXPECT_TRUE(refuses([&](auto& c) { c.topic_type = "event"; c.event = event; c.event->schedule.end_time = time; })) << time;
    }
    EXPECT_TRUE(refuses([&](auto& c) {
        c.topic_type = "event";
        c.event = event;
        c.event->recurrence = domain::LocalPostRecurrence{"yearly", "2027-01-31T00:00:00Z"};
    }));
    EXPECT_TRUE(refuses([](auto& c) { c.scheduled_time = "next Friday"; }));
    EXPECT_TRUE(refuses([](auto& c) { c.scheduled_time = "2026-12-19T17:00:00+08:00"; })) << "times are UTC";
    // A moment that does not exist is refused here, and not left for the table to refuse or to respell.
    for (const auto* moment : {"2026-12-19T17:00:60Z", "2027-02-30T17:00:00Z", "2027-02-29T17:00:00Z", "0000-12-19T17:00:00Z"}) {
        EXPECT_TRUE(refuses([&](auto& c) { c.scheduled_time = moment; })) << moment;
        EXPECT_TRUE(refuses([&](auto& c) {
            c.topic_type = "event";
            c.event = event;
            c.event->recurrence = domain::LocalPostRecurrence{"weekly", moment};
        })) << moment;
    }
    auto leap_day = standard();
    leap_day.scheduled_time = "2028-02-29T23:59:59.999999Z";
    EXPECT_TRUE(domain::normalize_local_post(leap_day)) << "a day the calendar has, to its last microsecond";
    EXPECT_TRUE(refuses([](auto& c) { c.media_ids = {999999}; })) << "an upload that does not exist";
    EXPECT_TRUE(refuses([](auto& c) { c.summary = std::string(6001, 'a'); }));
    EXPECT_EQ(count("local_posts"), 0);

    // An offer that says nothing about its offer is still an offer, with its three fields empty.
    auto offer = standard("This week only.");
    offer.topic_type = "offer";
    offer.event = event;
    const auto id = draft(spa_, offer);
    EXPECT_TRUE(shown(id).offer);
    EXPECT_FALSE(shown(id).offer->coupon_code);
}

TEST_F(LocalPostsPublish, TheTimeToPublishAtIsCheckedAgainWhenTheSendIsClaimed) {
    auto content = standard("Doors open in a moment.");
    content.scheduled_time = "2026-11-10T12:01:00Z";  // a minute from now
    const auto id = draft(spa_, content);
    ASSERT_TRUE(approve(id));
    clock_.advance(2min);
    run_jobs();
    EXPECT_TRUE(google_.creates.empty()) << "Google would refuse a time in the past, and the engine does not ask";
    EXPECT_EQ(post(id, "status"), "draft");
    EXPECT_THAT(post(id, "status_reason"), HasSubstr("scheduled_in_future"));
    EXPECT_EQ(failed_checks(id), "scheduled_in_future");
}

TEST_F(LocalPostsPublish, APhotoThatGoogleCouldNotFetchBlocksTheApproval) {
    auto content = standard("With a picture.");
    content.media_ids = {upload("p/old.jpg", "361 days")};
    const auto old = draft(spa_, content);
    EXPECT_EQ(failed_checks(old), "media_usable") << "the bucket removes a file after a year";
    EXPECT_EQ(approve(old).error().code, "checks_failed");

    // Without a public bucket (or with a folder standing in for it) there is no address Google can read.
    content.media_ids = {upload("p/new.jpg")};
    const auto created = as_staff_without_ip([&](pqxx::work& tx, audit::Writer& audit) {
        return repo::create_local_post(tx, audit, spa_, content, "file:///tmp/public/");
    });
    ASSERT_TRUE(created);
    ASSERT_EQ(created->checks.size(), 12u);
    EXPECT_FALSE(created->checks.back().passed);
    EXPECT_THAT(created->checks.back().detail, HasSubstr("no public bucket"));
}

TEST_F(LocalPostsPublish, OnlyADraftCanBeEditedOrRejected) {
    const auto id = draft(spa_);
    approve_and_send(id);
    EXPECT_EQ(edit(id, standard("Changed my mind.")).error().code, "local_post_not_open")
        << "a post that is at Google is not edited as a draft: it gets an edit of its own (D-099)";
    EXPECT_EQ(reject(id).error().code, "local_post_not_open");
    EXPECT_EQ(post(id, "content->>'summary'"), "Fall specials start Monday.");
    EXPECT_EQ(edit(999999, standard()).error().kind, repo::Failure::Kind::not_found);

    // An edit that changes nothing leaves no trace.
    const auto other = draft(spa_, standard("Same."));
    ASSERT_TRUE(edit(other, standard("  Same.  ")));
    EXPECT_EQ(audited("local_post.edit"), 0);
    EXPECT_EQ(post(other, "edited_by"), "");
    EXPECT_FALSE(reject(other, std::string(501, 'x')));
    ASSERT_TRUE(reject(other));
    EXPECT_EQ(post(other, "status_reason"), "") << "a reason is optional";
}

// ---- a create that fails, and one whose outcome nobody knows (D-088 point 6) ------------------

TEST_F(LocalPostsPublish, AValidationErrorReturnsThePostToTheDraftsWithGooglesMessage) {
    const auto id = draft(spa_);
    google_.fail_create = gbp::ApiError{gbp::ApiError::Kind::other, 400,
                                        "HTTP 400 INVALID_ARGUMENT Request contains an invalid argument. (summary: Must be at most 1500 characters)"};
    approve_and_send(id);
    EXPECT_EQ(google_.creates.size(), 1u);
    EXPECT_EQ(post(id, "status"), "draft");
    EXPECT_THAT(post(id, "status_reason"), HasSubstr("summary: Must be at most 1500 characters"));
    EXPECT_THAT(post(id, "status_reason"), HasSubstr("Nothing was published"));
    EXPECT_EQ(post(id, "google_name"), "");
    EXPECT_EQ(problems(), 1);
    EXPECT_EQ(value<std::string>("SELECT subject FROM notification_deliveries"), "Google refused a post");
    EXPECT_EQ(count("jobs WHERE kind = 'local_posts.sync'"), 0) << "nothing was created, so there is nothing to look for";

    // The job does not try again; a person edits and approves again, and that is a new send.
    run_jobs();
    EXPECT_EQ(google_.creates.size(), 1u);
    google_.fail_create.reset();
    clock_.advance(1min);
    approve_and_send(id);
    EXPECT_EQ(google_.creates.size(), 2u);
    EXPECT_EQ(post(id, "status"), "sent");
    EXPECT_EQ(post(id, "status_reason"), "");
}

TEST_F(LocalPostsPublish, ARateLimitedCreateIsTriedAgainBecauseNothingWasCreated) {
    const auto id = draft(spa_);
    ASSERT_TRUE(approve(id));
    google_.fail_create = gbp::ApiError{gbp::ApiError::Kind::rate_limited, 429, "HTTP 429 RESOURCE_EXHAUSTED"};
    EXPECT_THROW(publish(id), std::runtime_error) << "the job runs again";
    EXPECT_EQ(post(id, "status"), "approved");
    EXPECT_EQ(problems(), 0);

    google_.fail_create = gbp::ApiError{gbp::ApiError::Kind::unauthenticated, 401, "HTTP 401 UNAUTHENTICATED"};
    EXPECT_THROW(publish(id), std::runtime_error);
    google_.fail_create.reset();
    publish(id);
    EXPECT_EQ(post(id, "status"), "sent");
    EXPECT_EQ(google_.creates.size(), 3u) << "two refusals, then the one that created the post";
    EXPECT_EQ(google_.posts[kSpaPosts].size(), 1u);
}

TEST_F(LocalPostsPublish, AfterTheLastRefusedAttemptThePostReturnsToTheDrafts) {
    const auto id = draft(spa_);
    ASSERT_TRUE(approve(id));
    google_.fail_create = gbp::ApiError{gbp::ApiError::Kind::rate_limited, 429, "HTTP 429 RESOURCE_EXHAUSTED"};
    publish(id, true);
    EXPECT_EQ(post(id, "status"), "draft");
    EXPECT_THAT(post(id, "status_reason"), HasSubstr("429"));
    EXPECT_EQ(problems(), 1);
}

TEST_F(LocalPostsPublish, ATimeoutAfterGoogleTookThePostIsNotSentAgainAndTheNextReadAdoptsIt) {
    const auto id = draft(spa_);
    google_.create_times_out = true;
    google_.timeout_after_storing = true;
    approve_and_send(id);
    EXPECT_EQ(google_.creates.size(), 1u);
    EXPECT_EQ(post(id, "status"), "unknown");
    EXPECT_THAT(post(id, "status_reason"), HasSubstr("not sent again"));
    EXPECT_EQ(problems(), 1);
    EXPECT_EQ(value<std::string>("SELECT subject FROM notification_deliveries"), "A post may or may not have reached Google");

    // The job runs again (at-least-once delivery), and a person presses nothing: no second create.
    google_.create_times_out = false;
    publish(id);
    publish(id, true);
    EXPECT_EQ(approve(id).error().code, "local_post_not_open");
    EXPECT_EQ(google_.creates.size(), 1u);

    // A minute on, the first read after the send finds it.
    clock_.advance(1min);
    run_jobs();
    EXPECT_EQ(post(id, "status"), "sent");
    EXPECT_EQ(post(id, "google_name"), name_of(kSpaPosts, 7001));
    EXPECT_EQ(count("local_posts"), 1) << "the engine's own post, not a second one made at Google";
    EXPECT_EQ(problems(), 1);
}

TEST_F(LocalPostsPublish, ATimeoutThatLostThePostFailsAfterFifteenMinutesAndIsNeverSentAgain) {
    const auto id = draft(spa_);
    google_.create_times_out = true;
    approve_and_send(id);
    ASSERT_EQ(post(id, "status"), "unknown");
    google_.create_times_out = false;

    clock_.advance(1min);
    run_jobs();
    clock_.advance(4min);
    run_jobs();
    EXPECT_EQ(post(id, "status"), "unknown");
    clock_.advance(10min);
    run_jobs();
    EXPECT_EQ(post(id, "status"), "failed");
    EXPECT_EQ(problems(), 2) << "told when it became unknown, and when it failed";
    EXPECT_EQ(google_.creates.size(), 1u);
    EXPECT_TRUE(google_.posts[kSpaPosts].empty());

    publish(id);
    EXPECT_EQ(approve(id).error().code, "local_post_not_open") << "a failed post is copied as a new draft, never sent again";
    EXPECT_EQ(google_.creates.size(), 1u);
}

TEST_F(LocalPostsPublish, OnlyAnAnswerThatSaysForCertainThatNothingWasCreatedIsARefusal) {
    using Kind = gbp::ApiError::Kind;
    // The kind of an error comes from the body's status word first; the HTTP status decides here.
    struct Case {
        gbp::ApiError error;
        const char* then;  // the post's status after one run of the job
    };
    const std::vector<Case> cases{
        {{Kind::rate_limited, 503, "HTTP 503 RESOURCE_EXHAUSTED"}, "unknown"},   // a 5xx, whatever its body says
        {{Kind::unauthenticated, 500, "HTTP 500 UNAUTHENTICATED"}, "unknown"},
        {{Kind::not_found, 502, "HTTP 502 NOT_FOUND"}, "unknown"},
        {{Kind::other, 409, "HTTP 409 ABORTED"}, "unknown"},
        {{Kind::other, 499, "HTTP 499 CANCELLED"}, "unknown"},
        {{Kind::other, 302, "HTTP 302"}, "unknown"},
        {{Kind::other, 400, "HTTP 400 INVALID_ARGUMENT"}, "draft"},              // Google said no before doing anything
        {{Kind::permission_denied, 403, "HTTP 403 PERMISSION_DENIED"}, "draft"},
        {{Kind::not_found, 404, "HTTP 404 NOT_FOUND"}, "draft"},
        {{Kind::not_configured, 0, "the GBP account is not connected"}, "approved"},  // no request went out; tried again
    };
    int n = 0;
    for (const auto& item : cases) {
        const auto id = draft(spa_, standard(std::format("Case {}.", ++n)));
        ASSERT_TRUE(approve(id));
        google_.fail_create = item.error;
        try {
            publish(id);
        } catch (const std::runtime_error&) {  // the job asks to be run again
        }
        EXPECT_EQ(post(id, "status"), item.then) << item.error.message;
        // Whatever it became, a second run sends nothing unless the first was a certain "no" worth retrying.
        google_.fail_create.reset();
        const auto before = google_.creates.size();
        publish(id);
        EXPECT_EQ(google_.creates.size(), before + (std::string(item.then) == "approved" ? 1 : 0)) << item.error.message;
    }
}

TEST_F(LocalPostsPublish, AnAnswerThatArrivesAfterTheSendWasGivenUpStillNamesThePost) {
    const auto id = draft(spa_, standard("Slow answer."));
    ASSERT_TRUE(approve(id));
    ASSERT_EQ(claim(id), Outcome::claimed);
    // The job is stuck (a slow token, a paused limiter). Reads take the send for unknown, then give it up.
    clock_.advance(6min);
    sync(spa_);
    ASSERT_EQ(post(id, "status"), "unknown");
    clock_.advance(10min);
    sync(spa_);
    ASSERT_EQ(post(id, "status"), "failed");

    // Then Google's "accepted" comes back after all.
    const auto created = test::make_local_post("111", kSpaPosts, "55", "Slow answer.", "PROCESSING", "2026-11-10T12:16:30Z");
    tx([&](pqxx::work& tx) { repo::local_post_send_accepted(tx, id, created, clock_.now()); });
    EXPECT_EQ(post(id, "status"), "sent");
    EXPECT_EQ(post(id, "google_name"), name_of(kSpaPosts, 55));

    // A refusal or an "unknown" that comes back late changes nothing that was settled.
    tx([&](pqxx::work& tx) {
        repo::local_post_send_refused(tx, id, "HTTP 400", false, clock_.now());
        repo::local_post_send_unknown(tx, id, "timeout", clock_.now());
    });
    EXPECT_EQ(post(id, "status"), "sent");
    EXPECT_EQ(post(id, "google_name"), name_of(kSpaPosts, 55));
}

TEST_F(LocalPostsPublish, AnApprovalIsCarriedOutPromptlyOrNotAtAll) {
    // The engine was down, or the queue was behind, or the database came back from a backup: the job runs
    // long after the approval. What a person approved half an hour ago is not sent without them.
    const auto id = draft(spa_);
    ASSERT_TRUE(approve(id));
    clock_.advance(31min);
    run_jobs();
    EXPECT_TRUE(google_.creates.empty());
    EXPECT_EQ(post(id, "status"), "draft");
    EXPECT_THAT(post(id, "status_reason"), HasSubstr("within 30 minutes"));

    // Within the time, the job's own retries still go out.
    ASSERT_TRUE(approve(id));
    clock_.advance(29min);
    run_jobs();
    EXPECT_EQ(google_.creates.size(), 1u);
    EXPECT_EQ(post(id, "status"), "sent");
}

TEST_F(LocalPostsPublish, TwoJobsForOnePostSendOnce) {
    const auto id = draft(spa_);
    ASSERT_TRUE(approve(id));
    // A second job for the same post, however it got into the queue.
    await(queue().enqueue({.kind = std::string(repo::kPublishLocalPostKind), .payload = boost::json::object{{"local_post_id", id}}}));
    run_jobs();
    EXPECT_EQ(google_.creates.size(), 1u);
    EXPECT_EQ(post(id, "status"), "sent");

    // A job for a post nobody approved sends nothing.
    const auto other = draft(spa_, standard("Not approved."));
    await(queue().enqueue({.kind = std::string(repo::kPublishLocalPostKind), .payload = boost::json::object{{"local_post_id", other}}}));
    run_jobs();
    EXPECT_EQ(google_.creates.size(), 1u);
    EXPECT_EQ(post(other, "status"), "draft");
}

TEST_F(LocalPostsPublish, TheWorkerTriesARefusedCreateAgainAndGivesUpAfterItsLastAttempt) {
    const auto id = draft(spa_);
    ASSERT_TRUE(approve(id));
    google_.fail_create = gbp::ApiError{gbp::ApiError::Kind::rate_limited, 429, "HTTP 429 RESOURCE_EXHAUSTED"};
    run_jobs();
    EXPECT_EQ(post(id, "status"), "approved") << "waiting for the job's next attempt";
    EXPECT_EQ(google_.creates.size(), 1u);
    // The queue's own waits between attempts: 30 seconds, then a minute, then two.
    for (int attempt = 2; attempt <= repo::kPublishLocalPostAttempts; ++attempt) {
        clock_.advance(5min);
        run_jobs();
    }
    EXPECT_EQ(google_.creates.size(), 4u) << "one create per attempt, each refused before anything was created";
    EXPECT_EQ(post(id, "status"), "draft") << "after the last attempt a person decides";
    EXPECT_THAT(post(id, "status_reason"), HasSubstr("429"));
    EXPECT_EQ(problems(), 1);
    clock_.advance(1h);
    run_jobs();
    EXPECT_EQ(google_.creates.size(), 4u);
}

TEST_F(LocalPostsPublish, WhenTheOutcomeCannotBeWrittenDownThePostStaysAsBeingSentAndIsNotSentAgain) {
    const auto id = draft(spa_);
    ASSERT_TRUE(approve(id));
    // The database refuses the bookkeeping after Google has created the post.
    exec("CREATE FUNCTION refuse_sent() RETURNS trigger AS $$ BEGIN RAISE EXCEPTION 'the disk is full'; END $$ LANGUAGE plpgsql");
    exec("CREATE TRIGGER refuse_sent BEFORE UPDATE ON local_posts FOR EACH ROW WHEN (NEW.status = 'sent') EXECUTE FUNCTION refuse_sent()");
    EXPECT_NO_THROW(publish(id)) << "the job must not ask to be run again: the create went out";
    EXPECT_EQ(google_.creates.size(), 1u);
    EXPECT_EQ(post(id, "status"), "sending");
    publish(id);
    publish(id, true);
    EXPECT_EQ(google_.creates.size(), 1u);

    // Later reads settle it: the post is at Google, and it is this send's.
    exec("DROP TRIGGER refuse_sent ON local_posts");
    clock_.advance(6min);
    sync(spa_);
    EXPECT_EQ(post(id, "status"), "sent");
    EXPECT_EQ(count("local_posts"), 1);
}

TEST_F(LocalPostsPublish, TheApprovedContentIsComparedFieldByFieldWhenTheSendIsClaimed) {
    auto content = standard("With a button.");
    content.call_to_action = domain::LocalPostCallToAction{.action_type = "learn_more", .url = "https://example.com/a"};
    content.scheduled_time = "2026-12-19T17:00:00Z";
    for (const auto* change :
         {R"(jsonb_set(content, '{call_to_action,url}', '"https://example.com/b"'))",
          R"(jsonb_set(content, '{scheduled_time}', '"2026-12-20T17:00:00.000000Z"'))", R"(jsonb_set(content, '{language_code}', '"es"'))",
          R"(jsonb_set(content, '{call_to_action,action_type}', '"book"'))",
          R"(content || '{"topic_type": "event", "event": {"title": "T", "schedule": {"start_date": "2026-12-19", "start_time": "10:00", )"
          R"("end_date": "2026-12-19", "end_time": "11:00"}, "recurrence": null}}')"}) {
        const auto id = draft(spa_, content);
        ASSERT_TRUE(approve(id));
        exec(std::format("UPDATE local_posts SET content = {} WHERE id = {}", change, id));
        run_jobs();
        EXPECT_EQ(post(id, "status"), "draft") << change;
    }
    EXPECT_TRUE(google_.creates.empty());

    // An edit ends the approval a draft once had: what was approved was other content.
    const auto id = draft(spa_, content);
    ASSERT_TRUE(approve(id));
    exec("UPDATE system_settings SET posting_enabled = false");
    run_jobs();
    ASSERT_EQ(post(id, "status"), "draft");
    EXPECT_NE(post(id, "approved_content"), "");
    ASSERT_TRUE(edit(id, standard("Rewritten.")));
    EXPECT_EQ(post(id, "approved_content"), "");
    EXPECT_EQ(post(id, "decided_by"), "");
}

TEST_F(LocalPostsPublish, TheTableKeepsGooglesNameOffAPostThatCouldStillBeSent) {
    const auto id = draft(spa_);
    EXPECT_THROW(exec(std::format("UPDATE local_posts SET google_name = 'accounts/111/locations/9001/localPosts/1' WHERE id = {}", id)),
                 pqxx::check_violation);
    ASSERT_TRUE(approve(id));
    EXPECT_THROW(exec(std::format("UPDATE local_posts SET google_name = 'accounts/111/locations/9001/localPosts/1' WHERE id = {}", id)),
                 pqxx::check_violation);
    EXPECT_THROW(exec(std::format("UPDATE local_posts SET sent_at = now() WHERE id = {}", id)), pqxx::check_violation);
}

TEST_F(LocalPostsPublish, A5xxAndAnAnswerThatCannotBeReadAreTreatedAsPossiblySent) {
    const auto unavailable = draft(spa_, standard("Five hundred."));
    google_.fail_create = gbp::ApiError{gbp::ApiError::Kind::unavailable, 503, "HTTP 503 UNAVAILABLE"};
    approve_and_send(unavailable);
    EXPECT_EQ(post(unavailable, "status"), "unknown");

    const auto unreadable = draft(buffet_, standard("Accepted, unreadable."));
    google_.fail_create = gbp::ApiError{gbp::ApiError::Kind::invalid_response, 200, "Google accepted the post, but its answer names no post"};
    approve_and_send(unreadable);
    EXPECT_EQ(post(unreadable, "status"), "unknown") << "the post exists; the engine does not know its name";

    const auto timed_out = draft(buffet_, standard("Request timeout."));
    google_.fail_create = gbp::ApiError{gbp::ApiError::Kind::other, 408, "HTTP 408"};
    approve_and_send(timed_out);
    EXPECT_EQ(post(timed_out, "status"), "unknown");

    EXPECT_EQ(google_.creates.size(), 3u);
    google_.fail_create.reset();
    run_jobs();
    for (const auto id : {unavailable, unreadable, timed_out}) publish(id);
    EXPECT_EQ(google_.creates.size(), 3u) << "none of them is sent again";
}

TEST_F(LocalPostsPublish, AReadThatRunsWhileASendIsUnderWayDoesNotTakeItsPostForOneMadeAtGoogle) {
    const auto id = draft(spa_, standard("In flight."));
    ASSERT_TRUE(approve(id));
    // The job has claimed the send and Google has created the post; the bookkeeping has not happened yet.
    ASSERT_EQ(claim(id), Outcome::claimed);
    auto created = test::make_local_post("111", kSpaPosts, "88", "In flight.", "PROCESSING", "2026-11-10T12:00:01Z");
    google_.add(kSpaPosts, created);
    const auto outcome = sync(spa_);
    EXPECT_EQ(outcome.added, 0);
    EXPECT_EQ(count("local_posts"), 1);
    EXPECT_EQ(post(id, "status"), "sending");

    // Then the job writes down what Google answered.
    tx([&](pqxx::work& tx) { repo::local_post_send_accepted(tx, id, created, clock_.now()); });
    EXPECT_EQ(post(id, "status"), "sent");
    EXPECT_EQ(post(id, "google_name"), name_of(kSpaPosts, 88));
    EXPECT_EQ(count("local_posts"), 1);

    // Even if a read had stored the post first (Google changed its text on the way, say), it stays one row.
    const auto other = draft(spa_, standard("Text Google rewrote."));
    ASSERT_TRUE(approve(other));
    ASSERT_EQ(claim(other), Outcome::claimed);
    auto rewritten = test::make_local_post("111", kSpaPosts, "89", "Text Google rewrote!", "PROCESSING", "2026-11-10T12:00:02Z");
    google_.add(kSpaPosts, rewritten);
    EXPECT_EQ(sync(spa_).added, 1);
    tx([&](pqxx::work& tx) { repo::local_post_send_accepted(tx, other, rewritten, clock_.now()); });
    EXPECT_EQ(count("local_posts"), 2);
    EXPECT_EQ(post(other, "google_name"), name_of(kSpaPosts, 89));
    EXPECT_EQ(post(other, "origin"), "system");
}

TEST_F(LocalPostsPublish, ARequestToRemoveThePostMadeInThoseSecondsIsNotLost) {
    const auto id = draft(spa_, standard("Text Google rewrote."));
    ASSERT_TRUE(approve(id));
    ASSERT_EQ(claim(id), Outcome::claimed);
    // A read stored the post as one made at Google, and a person asked at once for it to be removed.
    auto rewritten = test::make_local_post("111", kSpaPosts, "90", "Text Google rewrote!", "PROCESSING", "2026-11-10T12:00:02Z");
    google_.add(kSpaPosts, rewritten);
    sync(spa_);
    const auto twin = post_named(name_of(kSpaPosts, 90));
    {
        as_staff_without_ip([&](pqxx::work& tx, audit::Writer& audit) {
            return repo::request_local_post_removal(tx, audit, twin, test::kMediaBase).has_value();
        });
    }
    tx([&](pqxx::work& tx) { repo::local_post_send_accepted(tx, id, rewritten, clock_.now()); });
    EXPECT_EQ(count("local_posts"), 1);
    EXPECT_NE(post(id, "removal_requested_at"), "") << "the request moved to the row that stays";
    EXPECT_EQ(post(id, "removal_requested_by"), std::to_string(user_));
    // Its job names the row that is gone and deletes nothing; the request is then given back for the person to make again.
    run_jobs();
    EXPECT_TRUE(google_.deletes.empty());
    clock_.advance(16min);
    await(local_posts_->enqueue_due());
    EXPECT_EQ(post(id, "removal_requested_at"), "");
    EXPECT_EQ(post(id, "status"), "sent");
}

TEST_F(LocalPostsPublish, AnApprovalWhoseJobDiedReturnsToTheDrafts) {
    const auto id = draft(spa_);
    ASSERT_TRUE(approve(id));
    exec("DELETE FROM jobs WHERE kind = 'local_posts.publish'");
    clock_.advance(10min);
    await(local_posts_->enqueue_due());
    EXPECT_EQ(post(id, "status"), "approved") << "its job may only be late";
    clock_.advance(6min);
    await(local_posts_->enqueue_due());
    EXPECT_EQ(post(id, "status"), "draft");
    EXPECT_THAT(post(id, "status_reason"), HasSubstr("Approve again"));
    EXPECT_TRUE(google_.creates.empty());

    // An approval whose job is waiting for its next attempt is left alone.
    ASSERT_TRUE(approve(id));
    clock_.advance(20min);
    await(local_posts_->enqueue_due());
    EXPECT_EQ(post(id, "status"), "approved");
}

TEST_F(LocalPostsPublish, AJobForAPostThatWasDeletedSendsNothingAndEnds) {
    const auto id = draft(spa_);
    ASSERT_TRUE(as_staff([&](pqxx::work& tx, audit::Writer& audit) { return repo::delete_local_post(tx, audit, id); }));
    // A job for the deleted post, however it got into the queue: only a draft or a rejected post can
    // be deleted, and neither has a job that will send it.
    await(queue().enqueue({.kind = std::string(repo::kPublishLocalPostKind), .payload = boost::json::object{{"local_post_id", id}}}));
    run_jobs();
    EXPECT_TRUE(google_.creates.empty());
    EXPECT_EQ(count("jobs WHERE kind = 'local_posts.publish' AND status = 'succeeded' AND attempts = 1"), 1) << "it ends, and is not tried again";
}
