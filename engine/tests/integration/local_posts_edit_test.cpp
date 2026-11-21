// An edit of a post that is at Google, over a real PostgreSQL and a fake
// Google (D-099): how an edit is written, approved and sent, that the one
// request names exactly what differs, and every way an edit must not reach
// Google, or must not reach it twice.
//
// No edit was ever sent to the real Google. What the fake does with one (it
// replaces the fields the mask names and keeps the post's state) is a
// stand-in, not knowledge: the tests are about what the engine does with
// each answer, whatever Google turns out to answer.

#include "local_post_fixture.hpp"

#include <gmock/gmock.h>

#include <functional>

using namespace poni;
using namespace std::chrono_literals;
using test::content_sql;
using test::kBuffetPosts;
using test::kSpaPosts;
using test::make_local_post;
using test::standard;
using testing::ElementsAre;
using testing::HasSubstr;
using testing::IsEmpty;
using testing::Not;
using Kind = gbp::ApiError::Kind;
using Claim = repo::LocalPostEditClaim::Outcome;
using Change = std::function<void(domain::LocalPostContent&)>;

namespace {

constexpr std::string_view kNotMade = "A post could not be edited";
constexpr std::string_view kNotConfirmed = "An edit of a post may or may not have reached Google";

class LocalPostsEdit : public test::LocalPostTest {
protected:
    void SetUp() override {
        LocalPostTest::SetUp();
        // Off after installation; most tests are about what happens when a person has switched it on.
        exec("UPDATE system_settings SET posting_enabled = true");
    }

    // ---- posts that are at Google ----

    // A post made in Google's own interface, read by the sync.
    std::int64_t at_google(const char* location, gbp::LocalPost made) {
        made.raw = boost::json::value_from(made);
        const auto name = made.name;
        google_.add(location, std::move(made));
        sync(location == std::string(kSpaPosts) ? spa_ : buffet_);
        return post_named(name);
    }
    std::int64_t at_google(const char* location, int id, const std::string& summary, std::string_view state = "LIVE") {
        return at_google(location, make_local_post("111", location, std::to_string(id), summary, state));
    }

    // A post written here: drafted, approved, sent, taken in by Google and read back.
    std::int64_t sent(std::int64_t location, const domain::LocalPostContent& content = standard()) {
        const auto created =
            as_staff([&](pqxx::work& tx, audit::Writer& audit) { return repo::create_local_post(tx, audit, location, content, test::kMediaBase); });
        EXPECT_TRUE(created) << (created ? "" : created.error().message);
        if (!created) return 0;
        const auto approved = as_staff([&](pqxx::work& tx, audit::Writer& audit) {
            return repo::approve_local_post(tx, audit, created->id, created->content(), test::kMediaBase);
        });
        EXPECT_TRUE(approved) << (approved ? "" : approved.error().message);
        run_jobs();
        google_.finish_processing();
        sync(location);
        EXPECT_EQ(post(created->id, "status"), "sent");
        return created->id;
    }

    // Something changes at Google, outside the engine.
    void at_google_change(std::int64_t id, const std::function<void(gbp::LocalPost&)>& change) {
        auto* held = google_.find(post(id, "google_name"));
        ASSERT_NE(held, nullptr);
        change(*held);
        held->raw = boost::json::value_from(*held);
    }

    // ---- what people do ----

    model::LocalPost shown(std::int64_t id) {
        pqxx::work tx{conn()};
        return *repo::get_local_post(tx, id, test::kMediaBase);
    }
    // The post's content with `change` made to it: what a person sends as an edit.
    domain::LocalPostContent changed(std::int64_t id, const Change& change) {
        auto content = shown(id).content();
        change(content);
        return content;
    }
    repo::Result<model::LocalPost> write(std::int64_t id, const domain::LocalPostContent& content) {
        return as_staff([&](pqxx::work& tx, audit::Writer& audit) { return repo::write_local_post_edit(tx, audit, id, content, test::kMediaBase); });
    }
    repo::Result<model::LocalPost> write(std::int64_t id, const Change& change) { return write(id, changed(id, change)); }
    repo::Result<void> discard(std::int64_t id) {
        return as_staff([&](pqxx::work& tx, audit::Writer& audit) { return repo::discard_local_post_edit(tx, audit, id); });
    }
    repo::Result<model::LocalPost> approve(std::int64_t id, const domain::LocalPostContent& content) {
        return as_staff([&](pqxx::work& tx, audit::Writer& audit) { return repo::approve_local_post_edit(tx, audit, id, content, test::kMediaBase); });
    }
    // Approves the edit as it is stored, as a person who has read it does.
    repo::Result<model::LocalPost> approve(std::int64_t id) {
        const auto post = shown(id);
        if (!post.edit) return approve(id, post.content());
        return approve(id, post.edit->content);
    }
    repo::Result<model::LocalPost> request_removal(std::int64_t id) {
        return as_staff([&](pqxx::work& tx, audit::Writer& audit) { return repo::request_local_post_removal(tx, audit, id, test::kMediaBase); });
    }

    // An edit written and approved; nothing has run yet. A moment has passed since whatever came before:
    // an approval's job is named by the post and the time of the approval.
    void approved_edit(std::int64_t id, const Change& change) {
        clock_.advance(1s);
        const auto written = write(id, change);
        ASSERT_TRUE(written) << written.error().code << ": " << written.error().message;
        const auto approved = approve(id);
        ASSERT_TRUE(approved) << approved.error().code << ": " << approved.error().message;
    }
    // ... and carried out by the worker.
    void edit_and_send(std::int64_t id, const Change& change) {
        approved_edit(id, change);
        run_jobs();
    }

    // ---- the job, step by step ----

    void run_edit(std::int64_t id, bool last_attempt = false) { await(local_posts_->edit(id, last_attempt)); }
    repo::LocalPostEditClaim claim(std::int64_t id) {
        return tx([&](pqxx::work& tx) { return repo::claim_local_post_edit(tx, id, clock_.now(), test::kMediaBase); });
    }
    repo::LocalPostEditRequest prepare(std::int64_t id, std::int64_t claim, const gbp::LocalPost& read) {
        return tx([&](pqxx::work& tx) { return repo::prepare_local_post_edit(tx, id, claim, read, clock_.now(), test::kMediaBase); });
    }
    gbp::LocalPost read_from_google(std::int64_t id) { return *google_.find(post(id, "google_name")); }

    // ---- what is there ----

    bool has_edit(std::int64_t id) { return count(std::format("local_post_edits WHERE local_post_id = {}", id)) == 1; }
    // A column of the post's edit, as text ("" for NULL, and for no edit).
    std::string edit(std::int64_t id, const std::string& column) {
        return value<std::string>(std::format("SELECT coalesce((SELECT {}::text FROM local_post_edits WHERE local_post_id = {}), '')", column, id));
    }
    std::string summary(std::int64_t id) { return post(id, "(content->>'summary')"); }
    // The clock's now as the API spells a time.
    std::string now_text() { return std::format("{:%FT%T}Z", clock_.now()); }
    std::string last_subject() { return value<std::string>("SELECT subject FROM notification_deliveries ORDER BY id DESC LIMIT 1"); }
    // The last audit entry of that action about the post.
    std::string audit_entry(std::int64_t id, const std::string& action, const std::string& expression) {
        return value<std::string>(std::format("SELECT coalesce(({})::text, '') FROM audit_log WHERE action = '{}' AND object_id = '{}' "
                                              "ORDER BY id DESC LIMIT 1",
                                              expression, action, id));
    }

    std::int64_t upload(const std::string& object = "p/abc123.jpg", const std::string& age = "0 days") {
        return insert_upload({.object = object, .age = age});
    }

    static domain::LocalPostContent an_offer() {
        auto offer = standard("Ten percent off lunch.");
        offer.topic_type = "offer";
        offer.event = domain::LocalPostEvent{.title = "Lunch week", .schedule = {"2026-12-19", "11:00", "2026-12-26", "15:30"}, .recurrence = {}};
        offer.offer = domain::LocalPostOffer{.coupon_code = "LUNCH10", .redeem_online_url = std::nullopt, .terms_conditions = "Dine-in only."};
        return offer;
    }
};

}  // namespace

// ---- the happy path, state by state ------------------------------------------------------

TEST_F(LocalPostsEdit, ACorrectionOfAPostThatWasScheduledAndIsLiveStaysTheSamePost) {
    // The case the edit exists for: a post is scheduled, Google publishes it, and staff spot a wrong price.
    auto content = standard("Lunch special: two courses for $18.");
    content.scheduled_time = "2026-11-10T12:05:00Z";
    const auto id = sent(spa_, content);
    ASSERT_EQ(post(id, "google_state"), "scheduled");
    clock_.advance(10min);
    at_google_change(id, [](auto& p) { p.state = "LIVE"; });
    sync(spa_);
    ASSERT_EQ(post(id, "google_state"), "live");
    const auto before = shown(id);

    // Written: it is beside the post, and nothing has gone anywhere.
    const auto written = write(id, [](auto& c) { c.summary = "Lunch special: two courses for $16."; });
    ASSERT_TRUE(written) << written.error().message;
    ASSERT_TRUE(written->edit);
    EXPECT_EQ(written->edit->status, "draft");
    EXPECT_EQ(written->edit->content.summary, "Lunch special: two courses for $16.");
    EXPECT_EQ(written->edit->written_by, "staff@xvisionmediaus.com");
    EXPECT_FALSE(written->edit->decided_by);
    EXPECT_EQ(written->edit->checks.size(), 12u);
    for (const auto& check : written->edit->checks) EXPECT_TRUE(check.passed) << check.name << ": the time it was published at has passed, and that is no fault";
    EXPECT_EQ(written->summary, "Lunch special: two courses for $18.") << "the post itself still says what Google has";
    EXPECT_EQ(written->status, "sent");
    EXPECT_TRUE(google_.updates.empty()) << "an edit that is written goes nowhere";
    EXPECT_EQ(count("jobs WHERE kind = 'local_posts.edit'"), 0);

    // Approved: still nothing at Google, and one job.
    const auto approved = approve(id);
    ASSERT_TRUE(approved) << approved.error().message;
    EXPECT_EQ(approved->edit->status, "approved");
    EXPECT_EQ(approved->edit->decided_by, "staff@xvisionmediaus.com");
    EXPECT_TRUE(google_.updates.empty()) << "the approval itself sends nothing";
    EXPECT_EQ(count("jobs WHERE kind = 'local_posts.edit'"), 1);
    EXPECT_EQ(summary(id), "Lunch special: two courses for $18.");

    // Sent: one request, for the one field that differs.
    const auto reads = google_.get_calls;
    run_jobs();
    EXPECT_EQ(google_.get_calls, reads + 1) << "the post is read from Google before it is changed";
    ASSERT_EQ(google_.updates.size(), 1u);
    EXPECT_EQ(google_.updates[0].name, name_of(kSpaPosts, 7001));
    EXPECT_THAT(google_.updates[0].mask, ElementsAre("summary"));
    EXPECT_EQ(google_.updates[0].body, boost::json::parse(R"({"summary": "Lunch special: two courses for $16."})").as_object());
    EXPECT_TRUE(google_.creates.size() == 1 && google_.deletes.empty()) << "no second post, and nothing removed";

    // The same post, with its history, and the new text.
    const auto after = shown(id);
    EXPECT_EQ(after.summary, "Lunch special: two courses for $16.");
    EXPECT_FALSE(after.edit) << "an edit Google accepted is no longer open";
    EXPECT_EQ(after.edited_by, "staff@xvisionmediaus.com");
    EXPECT_EQ(after.edited_at, now_text());
    EXPECT_EQ(after.id, before.id);
    EXPECT_EQ(after.google_name, before.google_name);
    EXPECT_EQ(after.status, "sent");
    EXPECT_EQ(after.origin, "system");
    EXPECT_EQ(after.scheduled_time, before.scheduled_time) << "it was a scheduled post, and still says when";
    EXPECT_EQ(after.sent_at, before.sent_at);
    EXPECT_EQ(after.created_by, before.created_by);
    EXPECT_EQ(after.decided_by, before.decided_by);
    EXPECT_EQ(after.decided_at, before.decided_at) << "when it was first approved, and by whom";
    EXPECT_EQ(after.created_at, before.created_at);
    EXPECT_EQ(after.google_state, "live") << "whatever Google answers; this Google leaves the state alone";
    EXPECT_EQ(count("local_posts"), 1);
    EXPECT_EQ(count("local_post_edits"), 0);
    EXPECT_EQ(problems(), 0);

    // Who did what: the person wrote and approved, the engine sent; the entry keeps the content before and after.
    EXPECT_EQ(audit_entry(id, "local_post.edit_write", "actor"), "staff@xvisionmediaus.com");
    EXPECT_EQ(audit_entry(id, "local_post.edit_write", "after->'edit'->>'summary'"), "Lunch special: two courses for $16.");
    EXPECT_EQ(audit_entry(id, "local_post.edit_approve", "actor"), "staff@xvisionmediaus.com");
    EXPECT_EQ(audit_entry(id, "local_post.edit_approve", "after->'edit'->>'summary'"), "Lunch special: two courses for $16.");
    EXPECT_EQ(audit_entry(id, "local_post.edit_started", "actor"), "system:local-posts");
    EXPECT_EQ(audit_entry(id, "local_post.edit_applied", "actor"), "system:local-posts");
    EXPECT_EQ(audit_entry(id, "local_post.edit_applied", "before->'content'->>'summary'"), "Lunch special: two courses for $18.");
    EXPECT_EQ(audit_entry(id, "local_post.edit_applied", "after->'content'->>'summary'"), "Lunch special: two courses for $16.");
    EXPECT_EQ(audit_entry(id, "local_post.edit_applied", "after->'approved_content'->>'summary'"), "Lunch special: two courses for $16.");
    EXPECT_EQ(audit_entry(id, "local_post.edit_applied", "after->>'approved_by_user_id'"), std::to_string(user_));
    EXPECT_EQ(audit_entry(id, "local_post.edit_applied", "after->>'written_by_user_id'"), std::to_string(user_));
    EXPECT_EQ(audit_entry(id, "local_post.edit_applied", "after->'fields'"), R"(["summary"])");

    // The next read brings nothing else, and nothing is sent again.
    clock_.advance(1h);
    await(local_posts_->enqueue_due());
    run_jobs();
    EXPECT_EQ(google_.updates.size(), 1u);
    EXPECT_EQ(shown(id).edited_by, "staff@xvisionmediaus.com") << "a read does not undo who edited";
    EXPECT_EQ(summary(id), "Lunch special: two courses for $16.");
}

TEST_F(LocalPostsEdit, EachFieldAnEditMayChangeIsSentUnderItsOwnNameAndNoOther) {
    struct Case {
        domain::LocalPostContent post;
        Change change;
        std::vector<std::string> mask;
        // What the post's row says afterwards: an expression over its content, and its value.
        std::string expression;
        std::string then;
    };
    auto with_button = standard("Open late on Fridays.");
    with_button.call_to_action = domain::LocalPostCallToAction{.action_type = "book", .url = "https://example.com/book"};
    const std::vector<Case> cases{
        {with_button, [](auto& c) { c.summary = "Open late on Saturdays."; }, {"summary"}, "content->>'summary'", "Open late on Saturdays."},
        {with_button, [](auto& c) { c.call_to_action->url = "https://example.com/reserve"; }, {"callToAction"}, "content#>>'{call_to_action,url}'",
         "https://example.com/reserve"},
        {with_button, [](auto& c) { c.call_to_action.reset(); }, {"callToAction"}, "content->>'call_to_action'", ""},
        {with_button, [](auto& c) { c.language_code = "es"; }, {"languageCode"}, "content->>'language_code'", "es"},
        {an_offer(), [](auto& c) { c.event->title = "Lunch fortnight"; }, {"event"}, "content#>>'{event,title}'", "Lunch fortnight"},
        {an_offer(), [](auto& c) { c.event->schedule.end_date = "2027-01-02"; }, {"event"}, "content#>>'{event,schedule,end_date}'", "2027-01-02"},
        {an_offer(), [](auto& c) { c.offer->coupon_code = "LUNCH15"; }, {"offer"}, "content#>>'{offer,coupon_code}'", "LUNCH15"},
        // Two at once, and three: in the one order, each once.
        {with_button,
         [](auto& c) {
             c.call_to_action->action_type = "order";
             c.summary = "Order ahead on Fridays.";
         },
         {"summary", "callToAction"}, "content#>>'{call_to_action,action_type}'", "order"},
        {an_offer(),
         [](auto& c) {
             c.offer->terms_conditions = "Dine-in and takeaway.";
             c.event->title = "Lunch month";
             c.language_code = "en-US";
         },
         {"event", "offer", "languageCode"}, "content#>>'{offer,terms_conditions}'", "Dine-in and takeaway."},
    };
    for (const auto& item : cases) {
        const auto id = sent(buffet_, item.post);
        const auto sent_before = google_.updates.size();
        edit_and_send(id, item.change);
        ASSERT_EQ(google_.updates.size(), sent_before + 1) << item.then;
        const auto& update = google_.updates.back();
        EXPECT_EQ(update.mask, item.mask) << item.then;
        // The body holds what the mask names and nothing else (a field taken away is left out of it).
        for (const auto& member : update.body) {
            EXPECT_TRUE(std::ranges::contains(item.mask, std::string(member.key()))) << std::string_view(member.key()) << " in the body of " << item.then;
        }
        EXPECT_EQ(post(id, "(" + item.expression + ")"), item.then);
        EXPECT_FALSE(has_edit(id));
        EXPECT_EQ(post(id, "edited_by"), std::to_string(user_));
    }
    EXPECT_EQ(problems(), 0);
    EXPECT_EQ(google_.creates.size(), cases.size()) << "one create per post, and no other";
}

TEST_F(LocalPostsEdit, TheMaskIsWorkedOutAgainstThePostAsGoogleHasItWhenTheEditIsSent) {
    auto content = standard("Open late on Fridays.");
    content.call_to_action = domain::LocalPostCallToAction{.action_type = "book", .url = "https://example.com/book"};
    const auto id = sent(spa_, content);
    approved_edit(id, [](auto& c) { c.summary = "Open late on Saturdays."; });
    EXPECT_EQ(audit_entry(id, "local_post.edit_approve", "after->'fields'"), R"(["summary"])") << "as the engine saw the post at the approval";

    // Before the job runs, someone changes the button in Google's own interface. The approval was for the
    // whole content the person read, button included: that content is what Google is given, field for field.
    at_google_change(id, [](auto& p) { p.callToAction->url = "https://example.com/elsewhere"; });
    run_jobs();
    ASSERT_EQ(google_.updates.size(), 1u);
    EXPECT_THAT(google_.updates[0].mask, ElementsAre("summary", "callToAction"));
    EXPECT_EQ(google_.updates[0].body.at("callToAction"), boost::json::parse(R"({"actionType": "BOOK", "url": "https://example.com/book"})"));
    EXPECT_EQ(post(id, "(content#>>'{call_to_action,url}')"), "https://example.com/book") << "the row is what Google answered";
    EXPECT_EQ(summary(id), "Open late on Saturdays.");
    // The entry keeps what Google had a moment before, which is not what the row had held.
    EXPECT_EQ(audit_entry(id, "local_post.edit_applied", "before->'content'#>>'{call_to_action,url}'"), "https://example.com/elsewhere");
}

// ---- the photo or video ---------------------------------------------------------------------

TEST_F(LocalPostsEdit, APostMadeAtGoogleKeepsItsOwnPhotosWhenAnythingElseIsEdited) {
    auto made = make_local_post("111", kBuffetPosts, "777", "Now open late on Fridays.");
    made.callToAction = gbp::CallToAction{.actionType = "CALL", .url = std::nullopt};
    made.media = std::vector<gbp::PostMedia>{{.mediaFormat = "PHOTO", .googleUrl = "https://lh3.googleusercontent.com/p/a"},
                                             {.mediaFormat = "PHOTO", .googleUrl = "https://lh3.googleusercontent.com/p/b"}};
    const auto id = at_google(kBuffetPosts, made);
    ASSERT_EQ(post(id, "origin"), "google") << "a post made in Google's own interface can be edited too";
    ASSERT_EQ(post(id, "media_id"), "");

    edit_and_send(id, [](auto& c) { c.summary = "Now open late on Fridays and Saturdays."; });
    ASSERT_EQ(google_.updates.size(), 1u);
    EXPECT_THAT(google_.updates[0].mask, ElementsAre("summary")) << "an edit without an upload never names the media of such a post";
    EXPECT_FALSE(google_.updates[0].body.contains("media"));
    ASSERT_TRUE(google_.find(name_of(kBuffetPosts, 777))->media);
    EXPECT_EQ(google_.find(name_of(kBuffetPosts, 777))->media->size(), 2u) << "Google's own copies are untouched";
    EXPECT_EQ(post(id, "jsonb_array_length(google_media)"), "2");
    EXPECT_EQ(post(id, "origin"), "google");
    EXPECT_EQ(post(id, "edited_by"), std::to_string(user_));
    EXPECT_EQ(post(id, "created_by"), "") << "nobody here wrote it";

    // Nothing was fetched from Google's media hosts, and nothing uploaded: the engine wrote one request.
    EXPECT_EQ(count("local_post_media"), 0);

    // A person may give such a post an upload of the engine's: then, and only then, the media is named.
    const auto photo = upload("p/new.jpg");
    edit_and_send(id, [&](auto& c) { c.media_id = photo; });
    ASSERT_EQ(google_.updates.size(), 2u);
    EXPECT_THAT(google_.updates[1].mask, ElementsAre("media"));
    EXPECT_EQ(google_.updates[1].body.at("media"),
              boost::json::parse(R"([{"mediaFormat": "PHOTO", "sourceUrl": "https://storage.googleapis.com/poni-public-test/p/new.jpg"}])"));
    EXPECT_EQ(post(id, "media_id"), std::to_string(photo));
    EXPECT_EQ(post(id, "jsonb_array_length(google_media)"), "1");
}

TEST_F(LocalPostsEdit, AnotherUploadReplacesThePostsAndTakingItAwayEmptiesTheMedia) {
    auto content = standard("A look at the new room.");
    content.media_id = upload("p/room.jpg");
    const auto id = sent(spa_, content);

    // The post's own upload, named again, is no change: the file is at Google already and is not sent.
    edit_and_send(id, [](auto& c) { c.summary = "A look at the new rooms."; });
    ASSERT_EQ(google_.updates.size(), 1u);
    EXPECT_THAT(google_.updates[0].mask, ElementsAre("summary"));
    EXPECT_EQ(post(id, "media_id"), std::to_string(*content.media_id));

    const auto video = insert_upload({.object = "p/walk.mp4", .format = "video", .content_type = "video/mp4", .bytes = 9000000,
                                      .width = 1080, .height = 1920, .duration_ms = "12500"});
    edit_and_send(id, [&](auto& c) { c.media_id = video; });
    ASSERT_EQ(google_.updates.size(), 2u);
    EXPECT_THAT(google_.updates[1].mask, ElementsAre("media"));
    EXPECT_EQ(google_.updates[1].body,
              boost::json::parse(R"({"media": [{"mediaFormat": "VIDEO", "sourceUrl": "https://storage.googleapis.com/poni-public-test/p/walk.mp4"}]})")
                  .as_object());
    EXPECT_EQ(post(id, "media_id"), std::to_string(video)) << "which upload the post carries now";
    EXPECT_THAT(post(id, "google_media->0->>'url'"), HasSubstr("googleusercontent.com"));

    edit_and_send(id, [](auto& c) { c.media_id.reset(); });
    ASSERT_EQ(google_.updates.size(), 3u);
    EXPECT_THAT(google_.updates[2].mask, ElementsAre("media"));
    EXPECT_EQ(google_.updates[2].body, boost::json::parse(R"({"media": []})").as_object());
    EXPECT_EQ(post(id, "media_id"), "");
    EXPECT_EQ(post(id, "jsonb_array_length(google_media)"), "0");

    // An upload that does not exist is refused where the edit is written.
    const auto missing = write(id, [](auto& c) { c.media_id = 999999; });
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().kind, repo::Failure::Kind::invalid);
    EXPECT_THAT(missing.error().message, HasSubstr("media_id"));
}

TEST_F(LocalPostsEdit, AnUploadIsLookedAtOnlyWhenItIsNotThePostsOwn) {
    auto content = standard("A look at the new room.");
    content.media_id = upload("p/room.jpg");
    const auto id = sent(spa_, content);
    // A year on, the bucket has removed the file. It is at Google all the same, and a change of the text does not send it.
    exec("UPDATE local_post_media SET created_at = created_at - interval '400 days'");
    const auto text_only = write(id, [](auto& c) { c.summary = "A look at the new rooms."; });
    ASSERT_TRUE(text_only);
    for (const auto& check : text_only->edit->checks) EXPECT_TRUE(check.passed) << check.name;
    ASSERT_TRUE(approve(id));
    run_jobs();
    EXPECT_EQ(google_.updates.size(), 1u);

    // Another upload that old could not be fetched by Google, and blocks the approval as for a draft.
    const auto old = upload("p/old.jpg", "361 days");
    const auto with_old = write(id, [&](auto& c) { c.media_id = old; });
    ASSERT_TRUE(with_old) << "stored, with its failed check";
    EXPECT_FALSE(with_old->edit->checks.back().passed);
    EXPECT_EQ(with_old->edit->checks.back().name, "media_usable");
    const auto refused = approve(id);
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, "checks_failed");
    EXPECT_THAT(refused.error().message, HasSubstr("media_usable"));
    EXPECT_EQ(google_.updates.size(), 1u);
}

// ---- nothing to change ------------------------------------------------------------------------

TEST_F(LocalPostsEdit, AnEditThatSaysWhatThePostSaysCannotBeApproved) {
    const auto id = at_google(kSpaPosts, 1, "Open late on Fridays.");
    // It may be written (a person pressed save without a change); it has nothing to send.
    ASSERT_TRUE(write(id, [](auto&) {}));
    auto refused = approve(id);
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, "local_post_edit_unchanged");
    EXPECT_EQ(count("jobs WHERE kind = 'local_posts.edit'"), 0);

    // The same when Google came to say what the edit says before anyone approved it.
    ASSERT_TRUE(write(id, [](auto& c) { c.summary = "Open late on Saturdays."; }));
    at_google_change(id, [](auto& p) { p.summary = "Open late on Saturdays."; });
    sync(spa_);
    refused = approve(id);
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, "local_post_edit_unchanged");
    EXPECT_TRUE(google_.updates.empty());
    EXPECT_TRUE(has_edit(id)) << "a refused approval changes nothing; the person discards it";
    ASSERT_TRUE(discard(id));
    EXPECT_FALSE(has_edit(id));
}

TEST_F(LocalPostsEdit, WhenGoogleAlreadyHasTheContentTheEditIsDoneWithoutARequest) {
    const auto id = at_google(kSpaPosts, 1, "Open late on Fridays.");
    approved_edit(id, [](auto& c) { c.summary = "Open late on Saturdays."; });
    // Between the approval and the job, Google comes to hold exactly this content.
    at_google_change(id, [](auto& p) { p.summary = "Open late on Saturdays."; });
    run_jobs();
    EXPECT_TRUE(google_.updates.empty()) << "there is nothing to change";
    EXPECT_FALSE(has_edit(id));
    EXPECT_EQ(summary(id), "Open late on Saturdays.") << "the row took what was read";
    EXPECT_EQ(post(id, "edited_by"), std::to_string(user_));
    EXPECT_EQ(audit_entry(id, "local_post.edit_applied", "after->>'sent'"), "false");
    EXPECT_EQ(audit_entry(id, "local_post.edit_applied", "after->'fields'"), "[]");
    EXPECT_EQ(problems(), 0);
}

// ---- Google refuses (for certain) ---------------------------------------------------------------

TEST_F(LocalPostsEdit, ACertainRefusalLeavesThePostAsItWasAndGivesTheEditBackWithGooglesMessage) {
    const std::vector<gbp::ApiError> refusals{
        // What nobody could find out beforehand: a field Google will not take in the mask.
        {Kind::other, 400, "HTTP 400 INVALID_ARGUMENT Request contains an invalid argument. (update_mask: callToAction cannot be updated)"},
        {Kind::other, 400, "HTTP 400 INVALID_ARGUMENT Request contains an invalid argument. (summary: Must be at most 1500 characters)"},
        {Kind::permission_denied, 403, "HTTP 403 PERMISSION_DENIED The caller does not have permission"},
        {Kind::not_found, 404, "HTTP 404 NOT_FOUND Requested entity was not found."},
    };
    int n = 0;
    for (const auto& refusal : refusals) {
        const auto id = at_google(kSpaPosts, ++n, std::format("Post {} as it was.", n));
        approved_edit(id, [&](auto& c) { c.summary = std::format("Post {} as it should be.", n); });
        google_.fail_update = refusal;
        const auto told = problems();
        run_jobs();
        EXPECT_EQ(google_.updates.size(), static_cast<std::size_t>(n)) << refusal.message;
        EXPECT_EQ(edit(id, "status"), "draft") << refusal.message;
        EXPECT_THAT(edit(id, "status_reason"), HasSubstr(refusal.message)) << "the reason is stored where the app can show it";
        EXPECT_THAT(edit(id, "status_reason"), HasSubstr("The post is as it was"));
        EXPECT_EQ(edit(id, "content->>'summary'"), std::format("Post {} as it should be.", n)) << "the edit is kept for the person to change";
        EXPECT_EQ(summary(id), std::format("Post {} as it was.", n));
        EXPECT_EQ(post(id, "status"), "sent");
        EXPECT_EQ(post(id, "edited_by"), "");
        EXPECT_EQ(problems(), told + 1);
        EXPECT_EQ(last_subject(), kNotMade);
        EXPECT_EQ(audit_entry(id, "local_post.edit_refused", "after->>'will_retry'"), "false");

        // The job does not try again, by itself or when run again.
        google_.fail_update.reset();
        run_jobs();
        run_edit(id);
        run_edit(id, true);
        EXPECT_EQ(google_.updates.size(), static_cast<std::size_t>(n)) << refusal.message;
    }

    // A person changes the edit and approves again: that is a new approval and a new request.
    const auto id = post_named(name_of(kSpaPosts, 1));
    clock_.advance(1min);
    edit_and_send(id, [](auto& c) { c.summary = "Post 1, shorter."; });
    EXPECT_EQ(google_.updates.size(), refusals.size() + 1);
    EXPECT_EQ(summary(id), "Post 1, shorter.");
    EXPECT_FALSE(has_edit(id));
}

TEST_F(LocalPostsEdit, ThePersonIsToldWithTheIdsThatOpenThePost) {
    const auto id = at_google(kBuffetPosts, 5, "Closed on Mondays.");
    approved_edit(id, [](auto& c) { c.summary = "Closed on Tuesdays."; });
    google_.fail_update = gbp::ApiError{Kind::other, 400, "HTTP 400 INVALID_ARGUMENT"};
    run_jobs();
    ASSERT_EQ(problems(), 1);
    const auto told = [&](const std::string& column) {
        return value<std::string>("SELECT coalesce(" + column + "::text, '') FROM notification_deliveries ORDER BY id DESC LIMIT 1");
    };
    EXPECT_EQ(told("event_type"), "system_problem");
    EXPECT_EQ(told("subject"), kNotMade);
    EXPECT_EQ(told("location_id"), std::to_string(buffet_));
    EXPECT_EQ(told("local_post_id"), std::to_string(id));
    EXPECT_EQ(told("link_path"), std::format("/#/posts/{}", id));
    EXPECT_THAT(told("body"), HasSubstr("HTTP 400 INVALID_ARGUMENT"));
    EXPECT_THAT(told("subject"), Not(HasSubstr("Closed"))) << "a post's text is never in a subject";

    // The other event, with the same ids.
    clock_.advance(1min);
    ASSERT_TRUE(approve(id));
    google_.fail_update = gbp::ApiError{Kind::unavailable, 503, "HTTP 503 UNAVAILABLE"};
    run_jobs();
    ASSERT_EQ(problems(), 2);
    EXPECT_EQ(told("subject"), kNotConfirmed);
    EXPECT_EQ(told("location_id"), std::to_string(buffet_));
    EXPECT_EQ(told("local_post_id"), std::to_string(id));
    EXPECT_THAT(told("body"), HasSubstr("It is not sent again"));
}

TEST_F(LocalPostsEdit, ARateLimitOrATokenErrorIsTriedAgainBecauseNothingWasChanged) {
    const auto id = at_google(kSpaPosts, 1, "Open late on Fridays.");
    approved_edit(id, [](auto& c) { c.summary = "Open late on Saturdays."; });
    google_.fail_update = gbp::ApiError{Kind::rate_limited, 429, "HTTP 429 RESOURCE_EXHAUSTED"};
    EXPECT_THROW(run_edit(id), std::runtime_error) << "the job runs again";
    EXPECT_EQ(edit(id, "status"), "approved");
    EXPECT_EQ(problems(), 0);

    google_.fail_update = gbp::ApiError{Kind::unauthenticated, 401, "HTTP 401 UNAUTHENTICATED"};
    EXPECT_THROW(run_edit(id), std::runtime_error);
    // No request went out at all for want of a token.
    google_.fail_update = gbp::ApiError{Kind::not_configured, 0, "the GBP account is not connected"};
    EXPECT_THROW(run_edit(id), std::runtime_error);
    EXPECT_EQ(edit(id, "status"), "approved");
    EXPECT_EQ(summary(id), "Open late on Fridays.");

    google_.fail_update.reset();
    run_edit(id);
    EXPECT_FALSE(has_edit(id));
    EXPECT_EQ(summary(id), "Open late on Saturdays.");
    EXPECT_EQ(google_.updates.size(), 4u) << "three refusals, then the one that changed the post";
    EXPECT_EQ(problems(), 0);
}

TEST_F(LocalPostsEdit, TheWorkerTriesARefusedEditAgainAndGivesItBackAfterItsLastAttempt) {
    const auto id = at_google(kSpaPosts, 1, "Open late on Fridays.");
    approved_edit(id, [](auto& c) { c.summary = "Open late on Saturdays."; });
    google_.fail_update = gbp::ApiError{Kind::rate_limited, 429, "HTTP 429 RESOURCE_EXHAUSTED"};
    run_jobs();
    EXPECT_EQ(edit(id, "status"), "approved") << "waiting for the job's next attempt";
    EXPECT_EQ(google_.updates.size(), 1u);
    for (int attempt = 2; attempt <= repo::kEditLocalPostAttempts; ++attempt) {
        clock_.advance(5min);
        run_jobs();
    }
    EXPECT_EQ(google_.updates.size(), 4u) << "one request per attempt, each refused before anything was changed";
    EXPECT_EQ(edit(id, "status"), "draft") << "after the last attempt a person decides";
    EXPECT_THAT(edit(id, "status_reason"), HasSubstr("429"));
    EXPECT_EQ(problems(), 1);
    EXPECT_EQ(last_subject(), kNotMade);
    clock_.advance(1h);
    run_jobs();
    EXPECT_EQ(google_.updates.size(), 4u);
    EXPECT_EQ(summary(id), "Open late on Fridays.");
}

// ---- an outcome nobody knows: never sent again ----------------------------------------------------

TEST_F(LocalPostsEdit, OnlyAnAnswerThatSaysForCertainThatNothingChangedIsARefusal) {
    // The kind of an error comes from the body's status word first; the HTTP status decides here.
    struct Case {
        gbp::ApiError error;
        const char* then;  // the edit's status after one run of the job
        bool uncertain;
    };
    const std::vector<Case> cases{
        {{Kind::rate_limited, 503, "HTTP 503 RESOURCE_EXHAUSTED"}, "draft", true},  // a 5xx, whatever its body says
        {{Kind::unauthenticated, 500, "HTTP 500 UNAUTHENTICATED"}, "draft", true},
        {{Kind::not_found, 502, "HTTP 502 NOT_FOUND"}, "draft", true},
        {{Kind::unavailable, 503, "HTTP 503 UNAVAILABLE"}, "draft", true},
        {{Kind::other, 408, "HTTP 408"}, "draft", true},
        {{Kind::other, 409, "HTTP 409 ABORTED"}, "draft", true},
        {{Kind::other, 499, "HTTP 499 CANCELLED"}, "draft", true},
        {{Kind::other, 302, "HTTP 302"}, "draft", true},
        {{Kind::invalid_response, 200, "Google accepted the edit, but its answer names no post"}, "draft", true},  // a 2xx that cannot be read
        {{Kind::other, 400, "HTTP 400 INVALID_ARGUMENT"}, "draft", false},  // Google said no before doing anything
        {{Kind::permission_denied, 403, "HTTP 403 PERMISSION_DENIED"}, "draft", false},
        {{Kind::not_found, 404, "HTTP 404 NOT_FOUND"}, "draft", false},
        {{Kind::rate_limited, 429, "HTTP 429 RESOURCE_EXHAUSTED"}, "approved", false},  // tried again
        {{Kind::not_configured, 0, "the GBP account is not connected"}, "approved", false},  // no request went out; tried again
    };
    int n = 0;
    for (const auto& item : cases) {
        const auto id = at_google(kSpaPosts, ++n, std::format("Case {} as it was.", n));
        approved_edit(id, [&](auto& c) { c.summary = std::format("Case {} as it should be.", n); });
        google_.fail_update = item.error;
        try {
            run_edit(id);
        } catch (const std::runtime_error&) {  // the job asks to be run again
        }
        EXPECT_EQ(edit(id, "status"), item.then) << item.error.message;
        EXPECT_EQ(summary(id), std::format("Case {} as it was.", n)) << "the row is never changed by guesswork";
        if (item.uncertain) {
            EXPECT_THAT(edit(id, "status_reason"), HasSubstr("Google did not confirm the edit")) << item.error.message;
            EXPECT_THAT(edit(id, "status_reason"), HasSubstr("may or may not have changed"));
            EXPECT_THAT(edit(id, "status_reason"), HasSubstr("Refresh from Google"));
            EXPECT_EQ(last_subject(), kNotConfirmed) << item.error.message;
            EXPECT_EQ(audit_entry(id, "local_post.edit_outcome_unknown", "after->'edit'->>'summary'"), std::format("Case {} as it should be.", n));
            EXPECT_EQ(audit_entry(id, "local_post.edit_outcome_unknown", "after->>'approved_by_user_id'"), std::to_string(user_));
        } else if (std::string_view(item.then) == "draft") {
            EXPECT_THAT(edit(id, "status_reason"), HasSubstr("The post is as it was")) << item.error.message;
        }
        // Whatever it became, a second run sends nothing unless the first was a certain "no" worth retrying.
        google_.fail_update.reset();
        const auto before = google_.updates.size();
        run_edit(id);
        run_edit(id, true);
        EXPECT_EQ(google_.updates.size(), before + (std::string_view(item.then) == "approved" ? 1 : 0)) << item.error.message;
    }
}

TEST_F(LocalPostsEdit, ATimeoutThatLostTheEditGivesItBackAndOnlyANewApprovalSendsAgain) {
    const auto id = at_google(kSpaPosts, 1, "Open late on Fridays.");
    approved_edit(id, [](auto& c) { c.summary = "Open late on Saturdays."; });
    google_.update_times_out = true;
    run_jobs();
    ASSERT_EQ(google_.updates.size(), 1u);
    EXPECT_EQ(edit(id, "status"), "draft");
    EXPECT_THAT(edit(id, "status_reason"), HasSubstr("Google did not confirm the edit"));
    EXPECT_EQ(post(id, "status"), "sent") << "an edit has no state of its own on the post: the post is at Google either way";
    EXPECT_EQ(problems(), 1);
    EXPECT_EQ(last_subject(), kNotConfirmed);

    // The job runs again (at-least-once delivery), reads follow, an hour passes: no second request.
    google_.update_times_out = false;
    run_edit(id);
    run_edit(id, true);
    clock_.advance(1h);
    await(local_posts_->enqueue_due());
    run_jobs();
    EXPECT_EQ(google_.updates.size(), 1u);
    EXPECT_EQ(edit(id, "status"), "draft") << "nothing settles an edit but a person";
    EXPECT_EQ(summary(id), "Open late on Fridays.") << "the read shows that it never arrived";

    // A person approves again: sending the same content again changes nothing that should not change.
    ASSERT_TRUE(approve(id));
    run_jobs();
    EXPECT_EQ(google_.updates.size(), 2u);
    EXPECT_FALSE(has_edit(id));
    EXPECT_EQ(summary(id), "Open late on Saturdays.");
}

TEST_F(LocalPostsEdit, ATimeoutAfterGoogleTookTheEditIsNotSentAgainAndTheNextReadShowsIt) {
    const auto id = at_google(kSpaPosts, 1, "Open late on Fridays.");
    approved_edit(id, [](auto& c) { c.summary = "Open late on Saturdays."; });
    google_.update_times_out = true;
    google_.timeout_after_updating = true;
    run_jobs();
    ASSERT_EQ(google_.updates.size(), 1u);
    EXPECT_EQ(edit(id, "status"), "draft");
    EXPECT_EQ(summary(id), "Open late on Fridays.") << "not changed by guesswork, though Google has the new text";
    google_.update_times_out = false;
    run_edit(id);
    EXPECT_EQ(google_.updates.size(), 1u);

    // "Refresh from Google" shows what it says now. The edit then has nothing left to send.
    sync(spa_);
    EXPECT_EQ(summary(id), "Open late on Saturdays.");
    EXPECT_TRUE(has_edit(id)) << "the read never touches an edit";
    EXPECT_EQ(approve(id).error().code, "local_post_edit_unchanged");
    ASSERT_TRUE(discard(id));
    EXPECT_EQ(google_.updates.size(), 1u);
}

TEST_F(LocalPostsEdit, ApprovingAgainBeforeAnyReadFindsTheEditAtGoogleAndSendsNothing) {
    const auto id = at_google(kSpaPosts, 1, "Open late on Fridays.");
    approved_edit(id, [](auto& c) { c.summary = "Open late on Saturdays."; });
    google_.update_times_out = true;
    google_.timeout_after_updating = true;
    run_jobs();
    ASSERT_EQ(edit(id, "status"), "draft");
    google_.update_times_out = false;

    clock_.advance(1min);
    ASSERT_TRUE(approve(id)) << "as far as the engine knows the post still says the old text";
    run_jobs();
    EXPECT_EQ(google_.updates.size(), 1u) << "the job read the post first, and Google had the content";
    EXPECT_FALSE(has_edit(id));
    EXPECT_EQ(summary(id), "Open late on Saturdays.");
    EXPECT_EQ(post(id, "edited_by"), std::to_string(user_));
}

TEST_F(LocalPostsEdit, AnAnswerAboutAnotherPostIsNoAnswerAboutThisOne) {
    const auto id = at_google(kSpaPosts, 1, "Open late on Fridays.");
    at_google(kSpaPosts, 2, "Another post.");
    approved_edit(id, [](auto& c) { c.summary = "Open late on Saturdays."; });
    for (const auto& name : {name_of(kSpaPosts, 2), name_of(kBuffetPosts, 1), name_of(kSpaPosts, 999)}) {
        google_.update_answers_as = name;
        const auto sent_before = google_.updates.size();
        run_jobs();
        EXPECT_EQ(google_.updates.size(), sent_before + 1);
        EXPECT_EQ(edit(id, "status"), "draft") << name;
        EXPECT_THAT(edit(id, "status_reason"), HasSubstr("did not confirm"));
        EXPECT_EQ(summary(id), "Open late on Fridays.") << "nothing of the answer is stored";
        EXPECT_EQ(summary(post_named(name_of(kSpaPosts, 2))), "Another post.");
        EXPECT_EQ(count("local_posts"), 2) << "and no row is made of it";
        // Put Google back as it was for the next round.
        at_google_change(id, [](auto& p) { p.summary = "Open late on Fridays."; });
        clock_.advance(1min);
        ASSERT_TRUE(approve(id));
    }
}

// ---- never twice, and never without a person ---------------------------------------------------

TEST_F(LocalPostsEdit, RunningTheEditJobAgainSendsNothing) {
    const auto id = at_google(kSpaPosts, 1, "Open late on Fridays.");
    edit_and_send(id, [](auto& c) { c.summary = "Open late on Saturdays."; });
    ASSERT_EQ(google_.updates.size(), 1u);
    run_edit(id);
    run_edit(id, true);
    EXPECT_EQ(google_.updates.size(), 1u);
    EXPECT_EQ(summary(id), "Open late on Saturdays.");
}

TEST_F(LocalPostsEdit, TwoJobsForOneEditSendOnce) {
    const auto id = at_google(kSpaPosts, 1, "Open late on Fridays.");
    approved_edit(id, [](auto& c) { c.summary = "Open late on Saturdays."; });
    // A second job for the same post, however it got into the queue.
    await(queue().enqueue({.kind = std::string(repo::kEditLocalPostKind), .payload = boost::json::object{{"local_post_id", id}}}));
    run_jobs();
    EXPECT_EQ(google_.updates.size(), 1u);
    EXPECT_FALSE(has_edit(id));
    EXPECT_EQ(count("jobs WHERE kind = 'local_posts.edit' AND status = 'succeeded'"), 2) << "the second ends without a word";
}

TEST_F(LocalPostsEdit, AnEditNobodyApprovedIsNeverSent) {
    const auto id = at_google(kSpaPosts, 1, "Open late on Fridays.");
    ASSERT_TRUE(write(id, [](auto& c) { c.summary = "Something nobody approved."; }));
    run_edit(id);
    run_edit(id, true);
    // A job that someone put into the queue by another way sends nothing either, nor does one for a post
    // without an edit, a draft, or a post that does not exist.
    const auto other = at_google(kSpaPosts, 2, "No edit here.");
    for (const auto target : {id, other, std::int64_t{999999}}) {
        await(queue().enqueue({.kind = std::string(repo::kEditLocalPostKind), .payload = boost::json::object{{"local_post_id", target}}}));
    }
    run_jobs();
    clock_.advance(1h);
    await(local_posts_->enqueue_due());
    run_jobs();
    EXPECT_TRUE(google_.updates.empty());
    EXPECT_EQ(edit(id, "status"), "draft");
    EXPECT_EQ(edit(id, "status_reason"), "");
    EXPECT_EQ(summary(id), "Open late on Fridays.");

    // The table itself refuses an edit on its way without a person and the content approved.
    EXPECT_THROW(exec(std::format("UPDATE local_post_edits SET status = 'approved' WHERE local_post_id = {}", id)), pqxx::check_violation);
    EXPECT_THROW(exec(std::format("UPDATE local_post_edits SET status = 'approved', decided_by = {}, decided_at = now() WHERE local_post_id = {}",
                                  user_, id)),
                 pqxx::check_violation)
        << "the approver alone is not enough: the content approved is kept";
    EXPECT_THROW(exec(std::format("UPDATE local_post_edits SET status = 'sending', decided_by = {}, decided_at = now(), approved_content = content "
                                  "WHERE local_post_id = {}",
                                  user_, id)),
                 pqxx::check_violation)
        << "an edit that was claimed says when, and under which claim";
    EXPECT_THROW(exec(std::format("UPDATE local_post_edits SET content = jsonb_set(content, '{{topic_type}}', '\"alert\"') WHERE local_post_id = {}", id)),
                 pqxx::check_violation);
    EXPECT_THROW(exec(std::format("INSERT INTO local_post_edits (local_post_id, status, content, written_by, written_at) "
                                  "SELECT local_post_id, 'draft', content, written_by, written_at FROM local_post_edits WHERE local_post_id = {}",
                                  id)),
                 pqxx::unique_violation)
        << "one open edit per post";
}

TEST_F(LocalPostsEdit, ContentThatIsNotTheContentApprovedIsNotSent) {
    auto content = standard("With a button.");
    content.call_to_action = domain::LocalPostCallToAction{.action_type = "learn_more", .url = "https://example.com/a"};
    int n = 0;
    // Whoever changed it, and however: the two values are compared as the row holds them, field by field.
    for (const auto* change : {R"(jsonb_set(content, '{summary}', '"Something nobody approved."'))",
                               R"(jsonb_set(content, '{call_to_action,url}', '"https://example.com/b"'))",
                               R"(jsonb_set(content, '{call_to_action,action_type}', '"book"'))", R"(jsonb_set(content, '{language_code}', '"es"'))",
                               R"(jsonb_set(content, '{media_id}', 'null') || '{"call_to_action": null}')"}) {
        auto made = make_local_post("111", kSpaPosts, std::to_string(++n), "With a button.");
        made.callToAction = gbp::CallToAction{.actionType = "LEARN_MORE", .url = "https://example.com/a"};
        const auto id = at_google(kSpaPosts, made);
        approved_edit(id, [](auto& c) { c.summary = "With a button, and a new text."; });
        exec(std::format("UPDATE local_post_edits SET content = {} WHERE local_post_id = {}", change, id));
        const auto reads = google_.get_calls;
        run_jobs();
        EXPECT_EQ(edit(id, "status"), "draft") << change;
        EXPECT_THAT(edit(id, "status_reason"), HasSubstr("not the content that was approved")) << change;
        EXPECT_EQ(google_.get_calls, reads) << "Google is not even asked";
        EXPECT_EQ(audit_entry(id, "local_post.edit_cancelled", "after->>'reason'"),
                  "The edit's content is not the content that was approved. Read it again and approve it.");
    }
    EXPECT_TRUE(google_.updates.empty());
    EXPECT_EQ(problems(), 0);
}

TEST_F(LocalPostsEdit, AnApprovalIsCarriedOutPromptlyOrNotAtAll) {
    // The engine was down, or the queue was behind, or the database came back from a backup: the job runs
    // long after the approval. What a person approved half an hour ago is not sent without them.
    const auto id = at_google(kSpaPosts, 1, "Open late on Fridays.");
    approved_edit(id, [](auto& c) { c.summary = "Open late on Saturdays."; });
    clock_.advance(31min);
    const auto reads = google_.get_calls;
    run_edit(id);
    EXPECT_TRUE(google_.updates.empty());
    EXPECT_EQ(google_.get_calls, reads);
    EXPECT_EQ(edit(id, "status"), "draft");
    EXPECT_THAT(edit(id, "status_reason"), HasSubstr("within 30 minutes"));

    // Within the time it goes out.
    ASSERT_TRUE(approve(id));
    clock_.advance(29min);
    run_edit(id);
    EXPECT_EQ(google_.updates.size(), 1u);
    EXPECT_FALSE(has_edit(id));
}

TEST_F(LocalPostsEdit, TwoWorkersThatReachTheClaimTogetherSendOnce) {
    const auto id = at_google(kSpaPosts, 1, "Open late on Fridays.");
    approved_edit(id, [](auto& c) { c.summary = "Open late on Saturdays."; });
    // The first worker has claimed and not yet committed; the second arrives and waits for the first worker's lock.
    const auto claim = [&](pqxx::work& tx) { return repo::claim_local_post_edit(tx, id, clock_.now(), test::kMediaBase); };
    const auto [mine, theirs] = race(claim, claim);
    ASSERT_EQ(mine.outcome, Claim::claimed);
    EXPECT_EQ(mine.name.str(), name_of(kSpaPosts, 1));
    EXPECT_EQ(theirs.outcome, Claim::not_approved);
    EXPECT_THAT(theirs.detail, HasSubstr("another run"));
    EXPECT_EQ(edit(id, "status"), "sending") << "the second leaves the first one's claim alone";
    EXPECT_EQ(problems(), 0);
}

// ---- the kill switches (D-088 point 5) -----------------------------------------------------

TEST_F(LocalPostsEdit, WithAPostingSwitchOffNoEditCanBeApproved) {
    const auto id = at_google(kSpaPosts, 1, "Open late on Fridays.");
    exec("UPDATE system_settings SET posting_enabled = false");
    ASSERT_TRUE(write(id, [](auto& c) { c.summary = "Open late on Saturdays."; })) << "an edit can still be written";
    auto refused = approve(id);
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, "posting_disabled");

    exec("UPDATE system_settings SET posting_enabled = true");
    exec(std::format("UPDATE locations SET posting_enabled = false WHERE id = {}", spa_));
    refused = approve(id);
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, "posting_disabled");
    EXPECT_THAT(refused.error().message, HasSubstr("for this location"));
    EXPECT_EQ(edit(id, "status"), "draft");
    EXPECT_EQ(count("jobs WHERE kind = 'local_posts.edit'"), 0);

    // The location's own switch stops that location only.
    const auto elsewhere = at_google(kBuffetPosts, 1, "Closed on Mondays.");
    edit_and_send(elsewhere, [](auto& c) { c.summary = "Closed on Tuesdays."; });
    EXPECT_EQ(google_.updates.size(), 1u);
}

TEST_F(LocalPostsEdit, TheSwitchesAreReadAgainInTheClaimAndAnEditNeverGoesOutLaterByItself) {
    const auto id = at_google(kSpaPosts, 1, "Open late on Fridays.");
    approved_edit(id, [](auto& c) { c.summary = "Open late on Saturdays."; });
    exec("UPDATE system_settings SET posting_enabled = false");
    const auto reads = google_.get_calls;
    run_jobs();
    EXPECT_TRUE(google_.updates.empty());
    EXPECT_EQ(google_.get_calls, reads);
    EXPECT_EQ(edit(id, "status"), "draft");
    EXPECT_THAT(edit(id, "status_reason"), HasSubstr("Posting is switched off"));
    EXPECT_EQ(audited("local_post.edit_cancelled"), 1);
    EXPECT_EQ(problems(), 0);

    // Switching posting back on sends nothing: a person approves again.
    exec("UPDATE system_settings SET posting_enabled = true");
    clock_.advance(1h);
    await(local_posts_->enqueue_due());
    run_jobs();
    run_edit(id);
    EXPECT_TRUE(google_.updates.empty());

    // The location's switch, flipped between the approval and the claim.
    ASSERT_TRUE(approve(id));
    exec(std::format("UPDATE locations SET posting_enabled = false WHERE id = {}", spa_));
    run_jobs();
    EXPECT_TRUE(google_.updates.empty());
    EXPECT_THAT(edit(id, "status_reason"), HasSubstr("for this location"));

    exec(std::format("UPDATE locations SET posting_enabled = true WHERE id = {}", spa_));
    clock_.advance(1min);
    ASSERT_TRUE(approve(id));
    run_jobs();
    EXPECT_EQ(google_.updates.size(), 1u);
    EXPECT_FALSE(has_edit(id));
}

// ---- what a person may approve ------------------------------------------------------------------

TEST_F(LocalPostsEdit, ApprovalIsForTheExactEditThePersonRead) {
    auto made = make_local_post("111", kSpaPosts, "1", "Holiday hours: open until nine.");
    made.callToAction = gbp::CallToAction{.actionType = "LEARN_MORE", .url = "https://example.com/hours"};
    const auto id = at_google(kSpaPosts, made);
    const auto first = write(id, [](auto& c) { c.summary = "Holiday hours: open until ten."; });
    ASSERT_TRUE(first);
    const auto read = first->edit->content;

    // Someone replaces the edit after it was shown.
    ASSERT_TRUE(write(id, [](auto& c) { c.summary = "Holiday hours: open until eleven."; }));
    const auto stale = approve(id, read);
    ASSERT_FALSE(stale);
    EXPECT_EQ(stale.error().code, "local_post_changed") << "nobody approves an edit they have not seen";

    // Every field counts, not only the text. Nor is the post's own content the edit.
    const auto stored = shown(id).edit->content;
    const auto photo = upload("p/other.jpg");
    for (const auto& change : std::vector<Change>{
             [](auto& c) { c.call_to_action->url = "https://example.com/other"; },
             [](auto& c) { c.call_to_action.reset(); },
             [](auto& c) { c.language_code = "es"; },
             [&](auto& c) { c.media_id = photo; },
             [](auto& c) { c.summary = "Holiday hours: open until nine."; },
         }) {
        auto other = stored;
        change(other);
        const auto refused = approve(id, other);
        ASSERT_FALSE(refused);
        EXPECT_EQ(refused.error().code, "local_post_changed");
    }
    EXPECT_EQ(edit(id, "status"), "draft");
    EXPECT_EQ(count("jobs WHERE kind = 'local_posts.edit'"), 0);

    // The same content written another way is the same content.
    auto same = stored;
    same.summary = "  Holiday hours: open until eleven.\n";
    ASSERT_TRUE(approve(id, same));
    run_jobs();
    ASSERT_EQ(google_.updates.size(), 1u);
    EXPECT_EQ(google_.updates[0].body.at("summary").as_string(), "Holiday hours: open until eleven.");

    // What is not a post at all is refused outright, with nothing looked up beyond the edit.
    ASSERT_TRUE(write(id, [](auto& c) { c.summary = "Again."; }));
    auto alert = shown(id).edit->content;
    alert.topic_type = "alert";
    EXPECT_EQ(approve(id, alert).error().kind, repo::Failure::Kind::invalid);
}

TEST_F(LocalPostsEdit, AFailedCheckIsStoredWithTheEditAndBlocksItsApproval) {
    const auto id = at_google(kSpaPosts, 1, "Use the button to book.");
    const auto with_phone = write(id, [](auto& c) { c.summary = "Call us on (212) 555-0100 to book."; });
    ASSERT_TRUE(with_phone) << "stored all the same";
    std::string failed;
    for (const auto& check : with_phone->edit->checks) {
        if (!check.passed) failed += check.name;
    }
    EXPECT_EQ(failed, "no_phone");
    const auto refused = approve(id);
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, "checks_failed");
    EXPECT_THAT(refused.error().message, HasSubstr("no_phone"));
    EXPECT_EQ(audit_entry(id, "local_post.edit_write", "after->'failed_checks'"), R"(["no_phone"])");

    // Replaced, checked again, approved.
    ASSERT_TRUE(write(id, [](auto& c) { c.summary = "Use the button to call us and book."; }));
    ASSERT_TRUE(approve(id));
    run_jobs();
    EXPECT_EQ(google_.updates.size(), 1u);

    // A button needs an https address in an edit as in a draft.
    const auto plain = write(id, [](auto& c) { c.call_to_action = domain::LocalPostCallToAction{.action_type = "book", .url = "http://example.com/book"}; });
    ASSERT_TRUE(plain);
    EXPECT_EQ(approve(id).error().code, "checks_failed");
    EXPECT_EQ(google_.updates.size(), 1u);
}

TEST_F(LocalPostsEdit, TheChecksRunAgainWhenTheEditIsClaimed) {
    const auto id = at_google(kSpaPosts, 1, "A look at the new room.");
    const auto photo = upload("p/room.jpg");
    approved_edit(id, [&](auto& c) { c.media_id = photo; });
    // The file is gone from the bucket's point of view before the job runs.
    exec("UPDATE local_post_media SET created_at = created_at - interval '400 days'");
    const auto reads = google_.get_calls;
    run_jobs();
    EXPECT_TRUE(google_.updates.empty()) << "Google could not fetch the file, and is not asked to";
    EXPECT_EQ(google_.get_calls, reads);
    EXPECT_EQ(edit(id, "status"), "draft");
    EXPECT_THAT(edit(id, "status_reason"), HasSubstr("media_usable"));
    EXPECT_THAT(edit(id, "checks"), HasSubstr("360 days")) << "the edit shows the check as it is now";
}

TEST_F(LocalPostsEdit, APostThatRepeatsWithoutAnEndOrWasScheduledInThePastCanStillBeEdited) {
    // Made at Google: an event that repeats and names no end, which a draft could not be approved with.
    auto made = make_local_post("111", kSpaPosts, "1", "Live music every Friday.");
    made.topicType = "EVENT";
    made.scheduledTime = "2026-11-01T17:00:00Z";
    made.event = gbp::PostEvent{.title = "Friday live music",
                                .schedule = gbp::TimeInterval{.startDate = gbp::Date{2026, 11, 6},
                                                              .startTime = gbp::TimeOfDay{18, std::nullopt},
                                                              .endDate = gbp::Date{2026, 11, 6},
                                                              .endTime = gbp::TimeOfDay{20, std::nullopt}},
                                .recurrenceInfo = gbp::RecurrenceInfo{.seriesEndTime = std::nullopt,
                                                                      .dailyPattern = std::nullopt,
                                                                      .weeklyPattern = boost::json::parse(R"({"daysOfWeek": ["FRIDAY"]})"),
                                                                      .monthlyPattern = std::nullopt}};
    const auto id = at_google(kSpaPosts, made);
    const auto written = write(id, [](auto& c) { c.event->title = "Friday jazz"; });
    ASSERT_TRUE(written) << written.error().message;
    for (const auto& check : written->edit->checks) EXPECT_TRUE(check.passed) << check.name;
    ASSERT_TRUE(approve(id));
    run_jobs();
    ASSERT_EQ(google_.updates.size(), 1u);
    EXPECT_THAT(google_.updates[0].mask, ElementsAre("event"));
    // The repeat goes back as Google holds it, weekdays included; the engine itself only knows "weekly".
    EXPECT_EQ(google_.updates[0].body.at("event").at("recurrenceInfo").at("weeklyPattern"), boost::json::parse(R"({"daysOfWeek": ["FRIDAY"]})"));
    EXPECT_EQ(google_.updates[0].body.at("event").at("title").as_string(), "Friday jazz");
    EXPECT_FALSE(google_.updates[0].body.contains("scheduledTime"));
    EXPECT_EQ(post(id, "(content#>>'{event,title}')"), "Friday jazz");
    EXPECT_EQ(post(id, "(content#>>'{event,recurrence,frequency}')"), "weekly");
    EXPECT_EQ(post(id, "(content->>'scheduled_time')"), "2026-11-01T17:00:00.000000Z");
}

// ---- what cannot have an edit, and what an edit cannot change -------------------------------------

TEST_F(LocalPostsEdit, OnlyAPostThatIsAtGoogleCanHaveAnEdit) {
    const auto row = [&](const std::string& status) {
        const auto content = content_sql(standard("A post."));
        const auto who = std::to_string(user_);
        if (status == "draft" || status == "rejected") {
            return insert_local_post(spa_, "origin, status, content, created_by", std::format("'system', '{}', {}, {}", status, content, who));
        }
        if (status == "removed") {
            return insert_local_post(spa_, "origin, status, content, google_name, removed_at",
                                     std::format("'google', 'removed', {}, 'accounts/111/locations/9001/localPosts/900', '2026-11-01T00:00:00Z'", content));
        }
        return insert_local_post(spa_, "origin, status, content, created_by, decided_by, approved_content, send_started_at",
                                 std::format("'system', '{}', {}, {}, {}, {}, '2026-11-01T00:00:00Z'", status, content, who, who, content));
    };
    for (const auto* status : {"draft", "rejected", "approved", "sending", "unknown", "failed", "removed"}) {
        const auto id = row(status);
        const auto refused = write(id, standard("Changed."));
        ASSERT_FALSE(refused) << status;
        EXPECT_EQ(refused.error().code, "local_post_not_editable") << status;
        EXPECT_EQ(approve(id, standard("Changed.")).error().code, "local_post_edit_not_open") << status;
        EXPECT_EQ(discard(id).error().code, "local_post_edit_not_open") << status;
    }
    EXPECT_EQ(write(999999, standard()).error().kind, repo::Failure::Kind::not_found);
    EXPECT_EQ(approve(999999, standard()).error().kind, repo::Failure::Kind::not_found);
    EXPECT_EQ(discard(999999).error().kind, repo::Failure::Kind::not_found);

    // A post whose location is not linked to an active client, or that the engine has lost access to.
    const auto id = at_google(kBuffetPosts, 1, "Closed on Mondays.");
    exec(std::format("UPDATE clients SET archived = true WHERE id = {}", buffet_client_));
    EXPECT_EQ(write(id, standard("Closed on Tuesdays.")).error().code, "location_not_synced");
    exec(std::format("UPDATE clients SET archived = false WHERE id = {}", buffet_client_));
    exec(std::format("UPDATE locations SET access_status = 'lost' WHERE id = {}", buffet_));
    EXPECT_EQ(write(id, standard("Closed on Tuesdays.")).error().code, "location_not_synced");
    exec(std::format("UPDATE locations SET access_status = 'active', client_id = NULL WHERE id = {}", buffet_));
    EXPECT_EQ(write(id, standard("Closed on Tuesdays.")).error().code, "location_not_synced");
    EXPECT_EQ(count("local_post_edits"), 0);
    EXPECT_EQ(count("audit_log WHERE action LIKE 'local_post.edit%'"), 0) << "a refused request leaves no entry";
}

TEST_F(LocalPostsEdit, APostThatHoldsWhatADraftCannotHoldIsNotEditedHere) {
    // An edit is content a draft can hold, so it could not say how to keep these parts; the engine does not
    // risk dropping them at Google.
    auto alert = make_local_post("111", kSpaPosts, "1", "Closed for the storm today.");
    alert.topicType = "ALERT";
    auto old_button = make_local_post("111", kSpaPosts, "2", "Get the offer.");
    old_button.callToAction = gbp::CallToAction{.actionType = "GET_OFFER", .url = "https://example.com/offer"};
    auto no_dates = make_local_post("111", kSpaPosts, "3", "An event Google holds without its dates.");
    no_dates.topicType = "EVENT";
    no_dates.event = gbp::PostEvent{.title = "Some day", .schedule = std::nullopt, .recurrenceInfo = std::nullopt};
    for (const auto& made : {alert, old_button, no_dates}) {
        const auto id = at_google(kSpaPosts, made);
        const auto refused = write(id, standard("A new text, and nothing else."));
        ASSERT_FALSE(refused) << made.name;
        EXPECT_EQ(refused.error().code, "local_post_not_editable") << made.name;
        EXPECT_THAT(refused.error().message, HasSubstr("cannot be edited here"));
    }
    EXPECT_EQ(count("local_post_edits"), 0);

    // A post that came to hold such a thing after an edit of it was approved is not written to either.
    const auto id = at_google(kSpaPosts, 4, "An ordinary post.");
    approved_edit(id, [](auto& c) { c.summary = "An ordinary post, corrected."; });
    at_google_change(id, [](auto& p) { p.callToAction = gbp::CallToAction{.actionType = "GET_OFFER", .url = "https://example.com/offer"}; });
    run_jobs();
    EXPECT_TRUE(google_.updates.empty());
    EXPECT_EQ(edit(id, "status"), "draft");
    EXPECT_THAT(edit(id, "status_reason"), HasSubstr("cannot be edited here"));
    EXPECT_THAT(edit(id, "status_reason"), HasSubstr("Nothing was sent"));
    EXPECT_EQ(post(id, "(content#>>'{call_to_action,action_type}')"), "get_offer") << "the row has what was read";
}

TEST_F(LocalPostsEdit, AnEditDoesNotChangeAPostsKindItsTimeOrItsRepeat) {
    auto made = make_local_post("111", kSpaPosts, "1", "Live music every Friday.", "RECURRING");
    made.topicType = "EVENT";
    made.scheduledTime = "2026-12-19T17:00:00Z";
    made.event = gbp::PostEvent{.title = "Friday live music",
                                .schedule = gbp::TimeInterval{.startDate = gbp::Date{2026, 12, 19},
                                                              .startTime = gbp::TimeOfDay{18, std::nullopt},
                                                              .endDate = gbp::Date{2026, 12, 19},
                                                              .endTime = gbp::TimeOfDay{20, std::nullopt}},
                                .recurrenceInfo = gbp::RecurrenceInfo{.seriesEndTime = "2027-01-31T00:00:00Z",
                                                                      .dailyPattern = std::nullopt,
                                                                      .weeklyPattern = boost::json::object{},
                                                                      .monthlyPattern = std::nullopt}};
    const auto id = at_google(kSpaPosts, made);
    const auto refused_member = [&](const Change& change) {
        const auto refused = write(id, change);
        EXPECT_FALSE(refused);
        if (refused) return std::string("(accepted)");
        EXPECT_EQ(refused.error().kind, repo::Failure::Kind::invalid) << "a 400 that names the member";
        return refused.error().message;
    };
    EXPECT_THAT(refused_member([](auto& c) { c.scheduled_time = "2026-12-20T17:00:00Z"; }), HasSubstr("\"scheduled_time\""));
    EXPECT_THAT(refused_member([](auto& c) { c.scheduled_time.reset(); }), HasSubstr("\"scheduled_time\""));
    EXPECT_THAT(refused_member([](auto& c) { c.event->recurrence->until = "2027-02-28T00:00:00Z"; }), HasSubstr("\"event.recurrence\""));
    EXPECT_THAT(refused_member([](auto& c) { c.event->recurrence->frequency = "daily"; }), HasSubstr("\"event.recurrence\""));
    EXPECT_THAT(refused_member([](auto& c) { c.event->recurrence.reset(); }), HasSubstr("\"event.recurrence\""));
    EXPECT_THAT(refused_member([](auto& c) {
                    c.topic_type = "offer";
                    c.offer = domain::LocalPostOffer{};
                }),
                HasSubstr("\"topic_type\""));
    EXPECT_THAT(refused_member([](auto& c) {
                    c.topic_type = "standard";
                    c.event.reset();
                }),
                HasSubstr("\"topic_type\""));
    // What is not a post at all is refused as for a draft.
    EXPECT_THAT(refused_member([](auto& c) { c.event->schedule.start_date = "2026-02-30"; }), HasSubstr("event.schedule"));
    EXPECT_EQ(count("local_post_edits"), 0);

    // Everything else of it is an edit's to change: the days and times with the repeat as it is.
    edit_and_send(id, [](auto& c) {
        c.event->schedule.start_time = "19:00";
        c.event->schedule.end_time = "21:00";
    });
    ASSERT_EQ(google_.updates.size(), 1u);
    EXPECT_THAT(google_.updates[0].mask, ElementsAre("event"));
    EXPECT_EQ(google_.updates[0].body.at("event").at("schedule").at("startTime"), boost::json::parse(R"({"hours": 19, "minutes": 0})"));
    EXPECT_EQ(post(id, "(content->>'scheduled_time')"), "2026-12-19T17:00:00.000000Z");
    EXPECT_EQ(post(id, "(content#>>'{event,recurrence,until}')"), "2027-01-31T00:00:00.000000Z");
}

TEST_F(LocalPostsEdit, AnEditWrittenForAPostThatThenChangedItsTimeAtGoogleIsNotSent) {
    auto made = make_local_post("111", kSpaPosts, "1", "Doors open at five.", "SCHEDULED");
    made.scheduledTime = "2026-12-19T17:00:00Z";
    const auto id = at_google(kSpaPosts, made);
    ASSERT_TRUE(write(id, [](auto& c) { c.summary = "Doors open at half past five."; }));

    // Someone moves the time in Google's own interface, and the read brings it.
    at_google_change(id, [](auto& p) { p.scheduledTime = "2026-12-20T17:00:00Z"; });
    sync(spa_);
    EXPECT_EQ(post(id, "(content->>'scheduled_time')"), "2026-12-20T17:00:00.000000Z") << "the read overwrites the post as ever";
    EXPECT_EQ(edit(id, "content->>'scheduled_time'"), "2026-12-19T17:00:00.000000Z") << "and never touches the edit";
    const auto refused = approve(id);
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, "local_post_changed");
    EXPECT_THAT(refused.error().message, HasSubstr("scheduled_time"));

    // The same when it moves between the approval and the job: the job reads the post first.
    ASSERT_TRUE(write(id, [](auto& c) { c.summary = "Doors open at half past five."; }));
    ASSERT_TRUE(approve(id));
    at_google_change(id, [](auto& p) { p.scheduledTime = "2026-12-21T17:00:00Z"; });
    run_jobs();
    EXPECT_TRUE(google_.updates.empty());
    EXPECT_EQ(edit(id, "status"), "draft");
    EXPECT_THAT(edit(id, "status_reason"), HasSubstr("changed at Google after the edit was written"));
    EXPECT_THAT(edit(id, "status_reason"), HasSubstr("scheduled_time"));
    EXPECT_EQ(post(id, "(content->>'scheduled_time')"), "2026-12-21T17:00:00.000000Z") << "the row has what was read";
    EXPECT_EQ(problems(), 0);
}

TEST_F(LocalPostsEdit, TheReadOverwritesThePostAndLeavesItsEditAlone) {
    const auto id = at_google(kSpaPosts, 1, "Open late on Fridays.");
    ASSERT_TRUE(write(id, [](auto& c) { c.language_code = "es"; }));
    const auto before = edit(id, "content");
    at_google_change(id, [](auto& p) {
        p.summary = "Open late on Fridays, and now on Saturdays.";
        p.state = "REJECTED";
    });
    clock_.advance(1h);
    sync(spa_);
    EXPECT_EQ(summary(id), "Open late on Fridays, and now on Saturdays.");
    EXPECT_EQ(post(id, "google_state"), "rejected");
    EXPECT_EQ(edit(id, "content"), before);
    EXPECT_EQ(edit(id, "status"), "draft");
    EXPECT_EQ(edit(id, "status_reason"), "");
    // The edit as it was written is still what a person approves: the whole content, old text included.
    ASSERT_TRUE(approve(id));
    run_jobs();
    ASSERT_EQ(google_.updates.size(), 1u);
    EXPECT_THAT(google_.updates[0].mask, ElementsAre("summary", "languageCode"));
    EXPECT_EQ(summary(id), "Open late on Fridays.");
}

// ---- two people ---------------------------------------------------------------------------------

TEST_F(LocalPostsEdit, ASecondPersonSeesTheOpenEditReplacesADraftAndWaitsForOneThatIsOnItsWay) {
    exec("INSERT INTO users (google_sub, email, name, created_at, last_login_at) "
         "VALUES ('sub-2', 'second@xvisionmediaus.com', 'Second Member', now(), now())");
    const auto second = value<std::int64_t>("SELECT id FROM users WHERE google_sub = 'sub-2'");
    const auto as_second = [&](auto f) { return transact(audit::Actor::user(second, "second@xvisionmediaus.com", "203.0.113.11"), f); };
    const auto id = at_google(kSpaPosts, 1, "Open late on Fridays.");
    ASSERT_TRUE(write(id, [](auto& c) { c.summary = "Open late on Saturdays."; }));

    // The open edit is on the post for everyone, with who wrote it.
    EXPECT_EQ(shown(id).edit->written_by, "staff@xvisionmediaus.com");
    // While it is a draft, another person's edit replaces it, and the edit then names them.
    clock_.advance(1min);
    const auto sundays = changed(id, [](auto& c) { c.summary = "Open late on Sundays."; });
    const auto replaced =
        as_second([&](pqxx::work& tx, audit::Writer& audit) { return repo::write_local_post_edit(tx, audit, id, sundays, test::kMediaBase); });
    ASSERT_TRUE(replaced) << replaced.error().message;
    EXPECT_EQ(replaced->edit->written_by, "second@xvisionmediaus.com");
    EXPECT_EQ(replaced->edit->written_at, now_text());
    EXPECT_EQ(replaced->edit->content.summary, "Open late on Sundays.");
    EXPECT_EQ(count("local_post_edits"), 1) << "one open edit per post";
    EXPECT_EQ(audit_entry(id, "local_post.edit_write", "before->'edit'->>'summary'"), "Open late on Saturdays.");
    EXPECT_EQ(audit_entry(id, "local_post.edit_write", "before->>'written_by_user_id'"), std::to_string(user_));
    EXPECT_EQ(audit_entry(id, "local_post.edit_write", "actor"), "second@xvisionmediaus.com");

    // The same content sent again changes nothing and leaves no entry.
    const auto entries = audited("local_post.edit_write");
    ASSERT_TRUE(write(id, [](auto& c) { c.summary = "  Open late on Sundays. "; }));
    EXPECT_EQ(audited("local_post.edit_write"), entries);
    EXPECT_EQ(shown(id).edit->written_by, "second@xvisionmediaus.com");

    // The first person approves what the second wrote. From then on it cannot be replaced or discarded.
    ASSERT_TRUE(approve(id));
    const auto every_day = changed(id, [](auto& c) { c.summary = "Open late every day."; });
    const auto late =
        as_second([&](pqxx::work& tx, audit::Writer& audit) { return repo::write_local_post_edit(tx, audit, id, every_day, test::kMediaBase); });
    ASSERT_FALSE(late);
    EXPECT_EQ(late.error().code, "local_post_edit_not_open");
    EXPECT_EQ(discard(id).error().code, "local_post_edit_not_open");
    EXPECT_EQ(approve(id).error().code, "local_post_edit_not_open") << "nor approved a second time";
    EXPECT_EQ(count("jobs WHERE kind = 'local_posts.edit'"), 1) << "one job per approval";
    ASSERT_EQ(claim(id).outcome, Claim::claimed);
    EXPECT_EQ(write(id, standard("While it is being sent.")).error().code, "local_post_edit_not_open");
    EXPECT_EQ(discard(id).error().code, "local_post_edit_not_open");
    EXPECT_EQ(edit(id, "content->>'summary'"), "Open late on Sundays.");
}

TEST_F(LocalPostsEdit, TheAuthorOfTheEditAndTheApproverAreBothKept) {
    exec("INSERT INTO users (google_sub, email, name, created_at, last_login_at) "
         "VALUES ('sub-2', 'second@xvisionmediaus.com', 'Second Member', now(), now())");
    const auto second = value<std::int64_t>("SELECT id FROM users WHERE google_sub = 'sub-2'");
    const auto id = sent(spa_, standard("Open late on Fridays."));
    const auto first_approval = post(id, "decided_at");
    clock_.advance(3min);
    const auto saturdays = changed(id, [](auto& c) { c.summary = "Open late on Saturdays."; });
    const auto written = transact(audit::Actor::user(second, "second@xvisionmediaus.com", ""), [&](pqxx::work& tx, audit::Writer& audit) {
        return repo::write_local_post_edit(tx, audit, id, saturdays, test::kMediaBase);
    });
    ASSERT_TRUE(written);
    clock_.advance(2min);
    ASSERT_TRUE(approve(id));
    clock_.advance(1min);
    run_jobs();

    const auto after = shown(id);
    EXPECT_EQ(after.edited_by, "second@xvisionmediaus.com") << "who wrote the edit";
    EXPECT_EQ(after.edited_at, now_text()) << "and when Google accepted it";
    EXPECT_EQ(after.created_by, "staff@xvisionmediaus.com");
    EXPECT_EQ(after.decided_by, "staff@xvisionmediaus.com") << "who approved the post in the first place";
    EXPECT_EQ(post(id, "decided_at"), first_approval);
    EXPECT_EQ(audit_entry(id, "local_post.edit_applied", "after->>'written_by_user_id'"), std::to_string(second));
    EXPECT_EQ(audit_entry(id, "local_post.edit_applied", "after->>'approved_by_user_id'"), std::to_string(user_));
}

TEST_F(LocalPostsEdit, ADiscardedEditIsGoneAndTheAuditLogKeepsWhatItSaid) {
    const auto id = at_google(kSpaPosts, 1, "Open late on Fridays.");
    ASSERT_TRUE(write(id, [](auto& c) { c.summary = "Open late on Saturdays."; }));
    ASSERT_TRUE(discard(id));
    EXPECT_FALSE(has_edit(id));
    EXPECT_FALSE(shown(id).edit);
    EXPECT_EQ(summary(id), "Open late on Fridays.");
    EXPECT_EQ(audit_entry(id, "local_post.edit_discard", "actor"), "staff@xvisionmediaus.com");
    EXPECT_EQ(audit_entry(id, "local_post.edit_discard", "before->'edit'->>'summary'"), "Open late on Saturdays.");
    EXPECT_EQ(audit_entry(id, "local_post.edit_discard", "before->>'written_by_user_id'"), std::to_string(user_));
    EXPECT_EQ(discard(id).error().code, "local_post_edit_not_open") << "there is none any more";
    EXPECT_TRUE(google_.updates.empty());
    EXPECT_EQ(post(id, "edited_by"), "") << "nothing was edited";

    // A new edit starts from nothing.
    ASSERT_TRUE(write(id, [](auto& c) { c.summary = "Open late on Sundays."; }));
    EXPECT_EQ(audit_entry(id, "local_post.edit_write", "before"), "") << "no edit before it";
}

// ---- the post is gone, or cannot be read ----------------------------------------------------------

TEST_F(LocalPostsEdit, WhenGoogleDoesNotFindThePostTheEditComesBackAndNothingIsMarkedRemoved) {
    const auto id = at_google(kSpaPosts, 1, "Open late on Fridays.");
    approved_edit(id, [](auto& c) { c.summary = "Open late on Saturdays."; });
    ASSERT_TRUE(google_.remove(name_of(kSpaPosts, 1)));
    run_jobs();
    EXPECT_TRUE(google_.updates.empty()) << "there is nothing to send an edit to";
    EXPECT_EQ(edit(id, "status"), "draft");
    EXPECT_THAT(edit(id, "status_reason"), HasSubstr("Google does not find the post"));
    EXPECT_THAT(edit(id, "status_reason"), HasSubstr("The edit was not sent"));
    EXPECT_EQ(post(id, "status"), "sent") << "the job marks nothing removed: the read does that, by its own rule";
    EXPECT_EQ(audited("local_post.gone_at_google"), 0);
    EXPECT_EQ(problems(), 1);
    EXPECT_EQ(last_subject(), kNotMade);
    run_edit(id);
    EXPECT_EQ(google_.get_calls, 1) << "not asked again: one read, by the job";

    // The read then finds it gone. A post that is removed keeps no approvable edit: it can only be discarded.
    clock_.advance(1h);
    sync(spa_);
    ASSERT_EQ(post(id, "status"), "removed");
    EXPECT_EQ(approve(id).error().code, "local_post_not_editable");
    EXPECT_EQ(write(id, standard("Too late.")).error().code, "local_post_not_editable");
    ASSERT_TRUE(discard(id));
    EXPECT_TRUE(google_.updates.empty());
}

TEST_F(LocalPostsEdit, APostThatCannotBeReadFirstIsNotWrittenTo) {
    const auto id = at_google(kSpaPosts, 1, "Open late on Fridays.");
    approved_edit(id, [](auto& c) { c.summary = "Open late on Saturdays."; });
    // Google is busy, down, or cannot be reached: a read is safe to repeat, so the job runs again.
    for (const auto& error : std::vector<gbp::ApiError>{{Kind::unavailable, 503, "HTTP 503 UNAVAILABLE"},
                                                        {Kind::rate_limited, 429, "HTTP 429 RESOURCE_EXHAUSTED"},
                                                        {Kind::unauthenticated, 401, "HTTP 401 UNAUTHENTICATED"},
                                                        {Kind::invalid_response, 200, "localPosts: the response is not JSON"}}) {
        google_.fail_get = error;
        EXPECT_THROW(run_edit(id), std::runtime_error) << error.message;
        EXPECT_EQ(edit(id, "status"), "approved") << error.message;
    }
    google_.fail_get.reset();
    google_.get_times_out = true;
    EXPECT_THROW(run_edit(id), std::runtime_error);
    EXPECT_EQ(edit(id, "status"), "approved");
    EXPECT_TRUE(google_.updates.empty());
    EXPECT_EQ(problems(), 0);
    EXPECT_EQ(audit_entry(id, "local_post.edit_not_sent", "after->>'will_retry'"), "true");

    // On the last attempt the edit comes back, and the person is told that nothing was sent.
    run_edit(id, true);
    EXPECT_EQ(edit(id, "status"), "draft");
    EXPECT_THAT(edit(id, "status_reason"), HasSubstr("could not be read from Google before the edit"));
    EXPECT_THAT(edit(id, "status_reason"), HasSubstr("nothing was sent"));
    EXPECT_EQ(problems(), 1);
    EXPECT_EQ(last_subject(), kNotMade);
    google_.get_times_out = false;

    // A plain "no" to the read is not tried again.
    clock_.advance(1min);
    ASSERT_TRUE(approve(id));
    google_.fail_get = gbp::ApiError{Kind::permission_denied, 403, "HTTP 403 PERMISSION_DENIED"};
    run_edit(id);
    EXPECT_EQ(edit(id, "status"), "draft");
    EXPECT_THAT(edit(id, "status_reason"), HasSubstr("403"));
    EXPECT_TRUE(google_.updates.empty());
    EXPECT_EQ(summary(id), "Open late on Fridays.");

    // The outbound client would not send the read at all (an address it does not take): the engine's own
    // failure, whose words stay in the log.
    google_.fail_get.reset();
    clock_.advance(1min);
    ASSERT_TRUE(approve(id));
    google_.client_refuses = true;
    run_edit(id, true);
    EXPECT_EQ(edit(id, "status"), "draft");
    EXPECT_THAT(edit(id, "status_reason"), HasSubstr("the engine could not prepare the request"));
    EXPECT_THAT(edit(id, "status_reason"), Not(HasSubstr("outbound client")));
    EXPECT_TRUE(google_.updates.empty());
}

// ---- the address -----------------------------------------------------------------------------------

TEST_F(LocalPostsEdit, APostIsEditedAsAPostOfItsOwnLocationOrNotAtAll) {
    // A row whose stored name belongs to another location: whatever wrote it, the engine does not follow it there.
    const auto misfiled = insert_local_post(spa_, "origin, status, content, google_name",
                                            "'google', 'sent', " + content_sql(standard("Misfiled.")) + ", 'accounts/111/locations/9002/localPosts/5'");
    google_.add(kBuffetPosts, make_local_post("111", kBuffetPosts, "5", "The buffet's own post."));
    approved_edit(misfiled, [](auto& c) { c.summary = "Misfiled, and edited."; });
    const auto reads = google_.get_calls;
    run_jobs();
    EXPECT_TRUE(google_.updates.empty());
    EXPECT_EQ(google_.get_calls, reads) << "not even read";
    EXPECT_EQ(edit(misfiled, "status"), "draft");
    EXPECT_THAT(edit(misfiled, "status_reason"), HasSubstr("not one of its location's"));
    EXPECT_EQ(google_.find(name_of(kBuffetPosts, 5))->summary, "The buffet's own post.");

    // A name that is no post's name at all.
    for (const auto* name : {"accounts/111/locations/9001", "accounts/111/locations/9001/localPosts/..", "accounts/111/locations/9001/reviews/r1"}) {
        ASSERT_TRUE(discard(misfiled));
        exec(std::format("UPDATE local_posts SET google_name = '{}' WHERE id = {}", name, misfiled));
        clock_.advance(1min);
        approved_edit(misfiled, [](auto& c) { c.summary = "Misfiled, and edited."; });
        run_jobs();
        EXPECT_EQ(edit(misfiled, "status"), "draft") << name;
    }
    EXPECT_TRUE(google_.updates.empty());
    EXPECT_EQ(google_.get_calls, reads);
}

TEST_F(LocalPostsEdit, TheRequestGoesUnderTheLocationsIdsAsTheyAreNow) {
    // The account's id as the location has it now: a location that moved to another account is still its location,
    // and its post is read and edited under the account that has it.
    const auto id = at_google(kSpaPosts, 3, "Read under the old account.");
    google_.account_id = "222";
    exec(std::format("UPDATE locations SET gbp_account_id = '222' WHERE id = {}", spa_));
    edit_and_send(id, [](auto& c) { c.summary = "Edited under the new account."; });
    ASSERT_EQ(google_.updates.size(), 1u);
    EXPECT_EQ(google_.updates[0].name, "accounts/222/locations/9001/localPosts/3");
    EXPECT_EQ(summary(id), "Edited under the new account.");
    EXPECT_EQ(count("local_posts"), 1) << "the answer is stored on the post's own row";
}

// ---- removal wins -------------------------------------------------------------------------------------

TEST_F(LocalPostsEdit, ARemovalIsNeverBlockedByAnEditAndADraftEditGoesWithIt) {
    const auto id = at_google(kSpaPosts, 1, "Scheduled by mistake.", "SCHEDULED");
    ASSERT_TRUE(write(id, [](auto& c) { c.summary = "Scheduled by mistake, and half corrected."; }));
    const auto requested = request_removal(id);
    ASSERT_TRUE(requested) << requested.error().message;
    EXPECT_FALSE(requested->edit) << "the draft edit is discarded with the request";
    EXPECT_FALSE(has_edit(id));
    EXPECT_EQ(audit_entry(id, "local_post.remove_requested", "after->'discarded_edit'->'content'->>'summary'"),
              "Scheduled by mistake, and half corrected.")
        << "and the entry keeps what it said";
    EXPECT_EQ(audit_entry(id, "local_post.remove_requested", "after->'discarded_edit'->>'written_by_user_id'"), std::to_string(user_));

    // While the removal is asked for, the post takes no new edit.
    const auto refused = write(id, standard("Too late."));
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, "local_post_not_editable");
    EXPECT_THAT(refused.error().message, HasSubstr("removed"));
    run_jobs();
    EXPECT_EQ(google_.deletes.size(), 1u);
    EXPECT_EQ(post(id, "status"), "removed");
    EXPECT_TRUE(google_.updates.empty());

    // A removal of a post without an edit says so too.
    const auto plain = at_google(kSpaPosts, 2, "No edit.");
    ASSERT_TRUE(request_removal(plain));
    EXPECT_EQ(audit_entry(plain, "local_post.remove_requested", "after->>'discarded_edit'"), "");
}

TEST_F(LocalPostsEdit, AnApprovedEditIsStoppedByARemovalThatWasAskedForMeanwhile) {
    const auto id = at_google(kSpaPosts, 1, "Scheduled by mistake.", "SCHEDULED");
    approved_edit(id, [](auto& c) { c.summary = "Scheduled by mistake, corrected."; });
    const auto requested = request_removal(id);
    ASSERT_TRUE(requested) << "the approved edit does not stand in its way";
    ASSERT_TRUE(requested->edit);
    EXPECT_EQ(requested->edit->status, "approved") << "its job stops it";

    run_jobs();
    EXPECT_TRUE(google_.updates.empty()) << "the claim saw the request";
    ASSERT_EQ(google_.deletes.size(), 1u);
    EXPECT_EQ(post(id, "status"), "removed");
    EXPECT_EQ(edit(id, "status"), "draft");
    EXPECT_THAT(edit(id, "status_reason"), HasSubstr("asked for this post to be removed"));
    EXPECT_EQ(approve(id).error().code, "local_post_not_editable") << "a removed post keeps no approvable edit";
    ASSERT_TRUE(discard(id));

    // A removal that Google refuses gives the post back, and with it an edit a person can approve again.
    const auto other = at_google(kSpaPosts, 2, "Staying after all.");
    approved_edit(other, [](auto& c) { c.summary = "Staying after all, corrected."; });
    ASSERT_TRUE(request_removal(other));
    google_.fail_delete = gbp::ApiError{Kind::permission_denied, 403, "HTTP 403 PERMISSION_DENIED"};
    run_jobs();
    ASSERT_EQ(post(other, "removal_requested_at"), "");
    EXPECT_TRUE(google_.updates.empty());
    clock_.advance(1min);
    ASSERT_TRUE(approve(other));
    run_jobs();
    EXPECT_EQ(google_.updates.size(), 1u);
    EXPECT_EQ(summary(other), "Staying after all, corrected.");
}

TEST_F(LocalPostsEdit, ARemovalAskedForWhileAnEditIsBeingSentIsNotHeldUpByIt) {
    // Claimed, and the request arrives before the job has read the post: the job asks again where the PATCH would start.
    const auto id = at_google(kSpaPosts, 1, "Scheduled by mistake.", "SCHEDULED");
    approved_edit(id, [](auto& c) { c.summary = "Scheduled by mistake, corrected."; });
    const auto claimed = claim(id);
    ASSERT_EQ(claimed.outcome, Claim::claimed);
    ASSERT_TRUE(request_removal(id)) << "never blocked, also while the edit is being sent";
    EXPECT_EQ(edit(id, "status"), "sending") << "the request does not reach into a run that is under way";
    const auto request = prepare(id, claimed.claim, read_from_google(id));
    EXPECT_EQ(request.outcome, repo::LocalPostEditRequest::Outcome::settled);
    EXPECT_TRUE(request.mask.empty());
    EXPECT_EQ(edit(id, "status"), "draft");
    EXPECT_THAT(edit(id, "status_reason"), HasSubstr("asked for this post to be removed"));
    run_jobs();
    EXPECT_TRUE(google_.updates.empty());
    EXPECT_EQ(google_.deletes.size(), 1u);
    EXPECT_EQ(post(id, "status"), "removed");

    // The request arrives while the PATCH is in the air: it cannot be called back. Its answer is written
    // down, the delete follows, and neither waits for the other.
    const auto other = at_google(kSpaPosts, 2, "Also by mistake.", "SCHEDULED");
    approved_edit(other, [](auto& c) { c.summary = "Also by mistake, corrected."; });
    const auto second = claim(other);
    ASSERT_EQ(second.outcome, Claim::claimed);
    const auto to_send = prepare(other, second.claim, read_from_google(other));
    ASSERT_EQ(to_send.outcome, repo::LocalPostEditRequest::Outcome::send);
    ASSERT_TRUE(request_removal(other));
    const auto answer = await(google_.update_local_post(second.name, to_send.mask, to_send.body));
    ASSERT_TRUE(answer);
    tx([&](pqxx::work& tx) { repo::local_post_edit_accepted(tx, other, second.claim, to_send.content, to_send.mask, *answer, clock_.now()); });
    EXPECT_FALSE(has_edit(other));
    EXPECT_NE(post(other, "removal_requested_at"), "") << "the request stands";
    run_jobs();
    EXPECT_EQ(google_.deletes.size(), 2u);
    EXPECT_EQ(post(other, "status"), "removed");
    EXPECT_EQ(audit_entry(other, "local_post.removed", "after->'content'->>'summary'"), "Also by mistake, corrected.")
        << "the entry of the removal keeps the post as it last was";
}

// ---- an engine that stopped, a job that died ---------------------------------------------------------

TEST_F(LocalPostsEdit, AnEditLeftAsBeingSentIsGivenBackAsNotConfirmedAndNeverSentAgain) {
    const auto id = at_google(kSpaPosts, 1, "Open late on Fridays.");
    approved_edit(id, [](auto& c) { c.summary = "Open late on Saturdays."; });
    // The engine stops between the claim and the bookkeeping: whether the request went out, nobody knows.
    ASSERT_EQ(claim(id).outcome, Claim::claimed);
    ASSERT_EQ(edit(id, "status"), "sending");

    // A run of the job a moment later finds it being sent, and sends nothing.
    run_edit(id);
    EXPECT_TRUE(google_.updates.empty());
    EXPECT_EQ(edit(id, "status"), "sending") << "it may be another run at work";
    clock_.advance(4min);
    await(local_posts_->enqueue_due());
    EXPECT_EQ(edit(id, "status"), "sending");

    // Five minutes after the claim no run is at work on it any more. The job that finds it so gives it back.
    clock_.advance(1min);
    run_edit(id);
    EXPECT_TRUE(google_.updates.empty()) << "it does not send";
    EXPECT_EQ(edit(id, "status"), "draft");
    EXPECT_THAT(edit(id, "status_reason"), HasSubstr("Google did not confirm the edit (the engine stopped while the edit was being sent)"));
    EXPECT_THAT(edit(id, "status_reason"), HasSubstr("may or may not have changed"));
    EXPECT_EQ(problems(), 1);
    EXPECT_EQ(last_subject(), kNotConfirmed);
    EXPECT_EQ(audited("local_post.edit_outcome_unknown"), 1);
    run_edit(id);
    run_edit(id, true);
    EXPECT_TRUE(google_.updates.empty());
    EXPECT_EQ(problems(), 1);

    // With no job left to find it, the sweep does the same.
    const auto other = at_google(kSpaPosts, 2, "Closed on Mondays.");
    approved_edit(other, [](auto& c) { c.summary = "Closed on Tuesdays."; });
    ASSERT_EQ(claim(other).outcome, Claim::claimed);
    exec("DELETE FROM jobs WHERE kind = 'local_posts.edit'");
    clock_.advance(5min);
    await(local_posts_->enqueue_due());
    EXPECT_EQ(edit(other, "status"), "draft");
    EXPECT_THAT(edit(other, "status_reason"), HasSubstr("Google did not confirm the edit"));
    EXPECT_EQ(problems(), 2);
    EXPECT_TRUE(google_.updates.empty());
}

TEST_F(LocalPostsEdit, AnApprovedEditWhoseJobDiedReturnsToTheDrafts) {
    const auto id = at_google(kSpaPosts, 1, "Open late on Fridays.");
    approved_edit(id, [](auto& c) { c.summary = "Open late on Saturdays."; });
    exec("DELETE FROM jobs WHERE kind = 'local_posts.edit'");
    clock_.advance(10min);
    await(local_posts_->enqueue_due());
    EXPECT_EQ(edit(id, "status"), "approved") << "its job may only be late";
    clock_.advance(6min);
    await(local_posts_->enqueue_due());
    EXPECT_EQ(edit(id, "status"), "draft");
    EXPECT_THAT(edit(id, "status_reason"), HasSubstr("Approve again"));
    EXPECT_TRUE(google_.updates.empty());
    EXPECT_EQ(problems(), 0) << "nothing was sent, so there is nothing in doubt";

    // An approval whose job is waiting for its next attempt is left alone.
    ASSERT_TRUE(approve(id));
    clock_.advance(20min);
    await(local_posts_->enqueue_due());
    EXPECT_EQ(edit(id, "status"), "approved");
}

TEST_F(LocalPostsEdit, WhenTheOutcomeCannotBeWrittenDownTheEditStaysAsBeingSentAndIsNotSentAgain) {
    const auto id = at_google(kSpaPosts, 1, "Open late on Fridays.");
    approved_edit(id, [](auto& c) { c.summary = "Open late on Saturdays."; });
    // The database refuses the bookkeeping after Google has changed the post.
    exec("CREATE FUNCTION refuse_edited() RETURNS trigger AS $$ BEGIN RAISE EXCEPTION 'the disk is full'; END $$ LANGUAGE plpgsql");
    exec("CREATE TRIGGER refuse_edited BEFORE UPDATE ON local_posts FOR EACH ROW WHEN (NEW.edited_at IS DISTINCT FROM OLD.edited_at) "
         "EXECUTE FUNCTION refuse_edited()");
    EXPECT_NO_THROW(run_edit(id)) << "the job must not ask to be run again: the request went out";
    EXPECT_EQ(google_.updates.size(), 1u);
    EXPECT_EQ(edit(id, "status"), "sending");
    EXPECT_EQ(summary(id), "Open late on Fridays.");
    run_edit(id);
    run_edit(id, true);
    EXPECT_EQ(google_.updates.size(), 1u);

    // Five minutes on it is given back as not confirmed; the next read shows that it arrived.
    exec("DROP TRIGGER refuse_edited ON local_posts");
    clock_.advance(6min);
    await(local_posts_->enqueue_due());
    EXPECT_EQ(edit(id, "status"), "draft");
    EXPECT_EQ(last_subject(), kNotConfirmed);
    sync(spa_);
    EXPECT_EQ(summary(id), "Open late on Saturdays.");
    EXPECT_EQ(google_.updates.size(), 1u);
}

TEST_F(LocalPostsEdit, AnAnswerThatComesAfterTheEditWasGivenBackStillSettlesIt) {
    const auto id = at_google(kSpaPosts, 1, "Slow answer.");
    approved_edit(id, [](auto& c) { c.summary = "Slow answer, corrected."; });
    const auto claimed = claim(id);
    ASSERT_EQ(claimed.outcome, Claim::claimed);
    const auto request = prepare(id, claimed.claim, read_from_google(id));
    ASSERT_EQ(request.outcome, repo::LocalPostEditRequest::Outcome::send);
    // The job is stuck (a slow answer, a paused limiter). The sweep takes the edit for one nobody will settle.
    clock_.advance(6min);
    await(local_posts_->enqueue_due());
    ASSERT_EQ(edit(id, "status"), "draft");
    ASSERT_EQ(problems(), 1);

    // A refusal or an "unknown" that comes back late changes nothing that was settled.
    tx([&](pqxx::work& tx) {
        repo::local_post_edit_not_made(tx, id, claimed.claim, repo::LocalPostEditFailure::refused, "HTTP 400", false, clock_.now());
        repo::local_post_edit_unknown(tx, id, claimed.claim, "timeout", clock_.now());
    });
    EXPECT_THAT(edit(id, "status_reason"), HasSubstr("did not confirm"));
    EXPECT_EQ(problems(), 1);

    // Then Google's "accepted" comes back after all: it says what the post is now, and the edit is done.
    const auto answer = await(google_.update_local_post(claimed.name, request.mask, request.body));
    ASSERT_TRUE(answer);
    tx([&](pqxx::work& tx) { repo::local_post_edit_accepted(tx, id, claimed.claim, request.content, request.mask, *answer, clock_.now()); });
    EXPECT_EQ(summary(id), "Slow answer, corrected.");
    EXPECT_FALSE(has_edit(id));
    EXPECT_EQ(post(id, "edited_by"), std::to_string(user_));

    // If a person has written another edit meanwhile, that one is theirs and stays.
    const auto other = at_google(kSpaPosts, 2, "Another slow answer.");
    approved_edit(other, [](auto& c) { c.summary = "Another slow answer, corrected."; });
    const auto second = claim(other);
    const auto to_send = prepare(other, second.claim, read_from_google(other));
    ASSERT_EQ(to_send.outcome, repo::LocalPostEditRequest::Outcome::send);
    clock_.advance(6min);
    await(local_posts_->enqueue_due());
    ASSERT_TRUE(write(other, [](auto& c) { c.summary = "Something else again."; }));
    const auto late = await(google_.update_local_post(second.name, to_send.mask, to_send.body));
    tx([&](pqxx::work& tx) { repo::local_post_edit_accepted(tx, other, second.claim, to_send.content, to_send.mask, *late, clock_.now()); });
    EXPECT_EQ(summary(other), "Another slow answer, corrected.") << "Google's answer is the post now";
    EXPECT_EQ(edit(other, "content->>'summary'"), "Something else again.");
    EXPECT_EQ(edit(other, "status"), "draft");

    // And a run's bookkeeping never settles an edit that another claim is sending.
    clock_.advance(1min);
    ASSERT_TRUE(approve(other));
    const auto third = claim(other);
    ASSERT_EQ(third.outcome, Claim::claimed);
    EXPECT_NE(third.claim, second.claim) << "a claim's number is never used twice";
    tx([&](pqxx::work& tx) {
        repo::local_post_edit_unknown(tx, other, second.claim, "timeout", clock_.now());
        repo::local_post_edit_not_made(tx, other, second.claim, repo::LocalPostEditFailure::refused, "HTTP 400", true, clock_.now());
    });
    EXPECT_EQ(edit(other, "status"), "sending");
    EXPECT_EQ(prepare(other, second.claim, read_from_google(other)).outcome, repo::LocalPostEditRequest::Outcome::settled);
    EXPECT_EQ(edit(other, "status"), "sending");
}

// ---- what stays as it was -------------------------------------------------------------------------------

TEST_F(LocalPostsEdit, ACopyTakesThePostsContentAndNotItsOpenEdit) {
    const auto id = at_google(kSpaPosts, 1, "Open late on Fridays.");
    ASSERT_TRUE(write(id, [](auto& c) { c.summary = "Open late on Saturdays."; }));
    const auto copy = as_staff([&](pqxx::work& tx, audit::Writer& audit) { return repo::copy_local_post(tx, audit, id, test::kMediaBase); });
    ASSERT_TRUE(copy);
    EXPECT_EQ(copy->summary, "Open late on Fridays.");
    EXPECT_FALSE(copy->edit);
    EXPECT_EQ(copy->status, "draft");
    EXPECT_TRUE(has_edit(id)) << "and the edit is still open";

    // A draft is edited itself, as ever; it has no edit beside it, and a post at Google is not edited that way.
    ASSERT_TRUE(as_staff([&](pqxx::work& tx, audit::Writer& audit) {
        return repo::edit_local_post(tx, audit, copy->id, standard("The copy, changed."), test::kMediaBase);
    }));
    EXPECT_EQ(write(copy->id, standard("Not this way.")).error().code, "local_post_not_editable");
    const auto refused = as_staff([&](pqxx::work& tx, audit::Writer& audit) {
        return repo::edit_local_post(tx, audit, id, standard("Not this way either."), test::kMediaBase);
    });
    EXPECT_EQ(refused.error().code, "local_post_not_open");
    // Nor is a post at Google deleted from the table because it has an edit, or without one.
    EXPECT_EQ(as_staff([&](pqxx::work& tx, audit::Writer& audit) { return repo::delete_local_post(tx, audit, id); }).error().code,
              "local_post_not_open");
    EXPECT_TRUE(google_.updates.empty());
    EXPECT_TRUE(google_.creates.empty());
}

TEST_F(LocalPostsEdit, AgainstTheGoogleMockNothingAboutEditsIsScheduled) {
    // The handlers exist, as the other post jobs' do, and nothing starts them: no schedule names the edit job.
    jobs::Registry mock;
    jobs::install(mock, *local_posts_, true);
    EXPECT_TRUE(mock.recurring().empty());
    jobs::Registry real;
    jobs::install(real, *local_posts_, false);
    for (const auto& recurring : real.recurring()) EXPECT_NE(recurring.kind, repo::kEditLocalPostKind) << "an edit starts with a person's approval only";
}
