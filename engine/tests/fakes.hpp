// Stand-ins for everything outside the engine, so tests never call Google
// or a model: a scripted HTTP client, an in-memory GBP account (reads and
// the reply to a review), a Pub/Sub subscription, an identity provider, an
// LLM, a notification channel, the Performance API, a bucket, and the local
// posts of that account (reads, create and delete).
#pragma once

#include "connectors/gbp/local_posts.hpp"
#include "connectors/gbp/performance.hpp"
#include "connectors/gbp/reader.hpp"
#include "connectors/gbp/writer.hpp"
#include "connectors/notify/apns.hpp"
#include "connectors/notify/channel.hpp"
#include "connectors/identity/provider.hpp"
#include "connectors/llm/client.hpp"
#include "connectors/pubsub/subscriber.hpp"
#include "connectors/storage/store.hpp"
#include "core/json.hpp"
#include "domain/local_post.hpp"
#include "http/client.hpp"

#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <boost/asio/error.hpp>
#include <boost/json/value_from.hpp>
#include <boost/system/system_error.hpp>

#include <algorithm>
#include <cstdint>
#include <deque>
#include <format>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace poni::test {

// Answers each request with the next scripted response and remembers the request.
class FakeHttp final : public http::Client {
public:
    void reply(unsigned status, std::string body) { responses_.push_back(http::ClientResponse{status, std::move(body)}); }
    // The next request goes out and no answer comes back: a transport error, as after a timeout.
    void fail() { responses_.push_back(std::nullopt); }

    boost::asio::awaitable<http::ClientResponse> send(http::ClientRequest request) override {
        // What the real client refuses to send is refused here too, and never
        // counted as sent: a connector that builds such a request fails its test.
        if (const auto refusal = http::why_refused(request)) {
            refused.push_back(std::move(request));
            throw std::invalid_argument(std::string(*refusal));
        }
        requests.push_back(std::move(request));
        if (responses_.empty()) co_return http::ClientResponse{500, R"({"error":"the test scripted no response"})"};
        auto response = std::move(responses_.front());
        responses_.pop_front();
        if (!response) throw boost::system::system_error(boost::asio::error::timed_out);
        co_return std::move(*response);
    }

    // As the real client: every request is checked first, and a failure
    // anywhere returns nothing. The answers are taken in order, one each.
    boost::asio::awaitable<std::vector<http::ClientResponse>> send_h2(std::vector<http::ClientRequest> batch) override {
        for (const auto& request : batch) {
            if (const auto refusal = http::why_refused(request)) {
                refused.push_back(request);
                throw std::invalid_argument(std::string(*refusal));
            }
        }
        h2_batches.push_back(batch.size());
        std::vector<http::ClientResponse> out;
        bool failed = false;
        for (auto& request : batch) {
            requests.push_back(std::move(request));
            if (responses_.empty()) {
                out.push_back(http::ClientResponse{500, R"({"error":"the test scripted no response"})"});
                continue;
            }
            auto response = std::move(responses_.front());
            responses_.pop_front();
            if (!response) failed = true;
            else out.push_back(std::move(*response));
        }
        if (failed) throw boost::system::system_error(boost::asio::error::timed_out);
        co_return out;
    }

    std::string header(std::size_t request_index, std::string_view name) const {
        for (const auto& [key, value] : requests.at(request_index).headers) {
            if (key == name) return value;
        }
        return {};
    }

    std::vector<http::ClientRequest> requests;
    // Requests the outbound client's rules refused (http/client.hpp).
    std::vector<http::ClientRequest> refused;
    // How many requests each send_h2 carried.
    std::vector<std::size_t> h2_batches;

private:
    std::deque<std::optional<http::ClientResponse>> responses_;  // nothing: no answer comes
};

inline gbp::Review make_review(std::string_view account_id, std::string_view location_id, std::string_view review_id,
                               std::string_view stars, std::optional<std::string> comment, std::string_view created,
                               std::string_view updated, std::optional<std::string> reply = std::nullopt,
                               std::string_view reply_updated = "2026-11-01T00:00:00Z") {
    gbp::Review review;
    review.name = std::format("accounts/{}/locations/{}/reviews/{}", account_id, location_id, review_id);
    review.reviewId = std::string(review_id);
    review.reviewer = gbp::Reviewer{.displayName = "Reviewer " + std::string(review_id), .profilePhotoUrl = std::nullopt, .isAnonymous = false};
    review.starRating = std::string(stars);
    review.comment = std::move(comment);
    review.createTime = std::string(created);
    review.updateTime = std::string(updated);
    if (reply) {
        review.reviewReply = gbp::ReviewReply{.comment = std::move(reply),
                                              .updateTime = std::string(reply_updated),
                                              .reviewReplyState = "APPROVED"};
    }
    review.raw = boost::json::value_from(review);
    return review;
}

// One GBP managing account held in memory. Lists are served newest first in
// pages, like Google does. A reply that is put is accepted and then held
// back, as Google does for a few minutes (spike B6): reads show it only
// after publish_held().
class FakeGbp final : public gbp::Reader, public gbp::ReplyWriter {
public:
    // ---- the write side ----
    struct Put {
        std::string review_id;
        std::string comment;
    };
    // Every PUT that reached "Google", refused ones included.
    std::vector<Put> puts;
    // When set, the PUT is refused with this error and nothing is stored.
    std::optional<gbp::ApiError> fail_put;
    // The connection drops after the request was sent: the caller gets a
    // transport error. With `timeout_after_storing` Google stored the reply anyway.
    bool put_times_out = false;
    bool timeout_after_storing = false;
    // Accepted replies that reads don't show yet: review id -> (location id, text).
    std::map<std::string, std::pair<std::string, std::string>> held;

    boost::asio::awaitable<gbp::Result<gbp::ReviewReply>> put_reply(gbp::ReviewName name, std::string comment) override {
        puts.push_back({name.review_id, comment});
        if (fail_put) co_return std::unexpected(*fail_put);
        if (put_times_out) {
            if (timeout_after_storing) held[name.review_id] = {name.location_id, comment};
            throw boost::system::system_error(boost::asio::error::timed_out);
        }
        held[name.review_id] = {name.location_id, comment};
        co_return gbp::ReviewReply{.comment = std::move(comment), .updateTime = "2026-11-10T12:00:01Z", .reviewReplyState = std::nullopt};
    }

    // Google starts showing the replies it held back.
    void publish_held(std::string_view reply_time = "2026-11-10T12:00:01Z") {
        for (const auto& [review_id, entry] : held) {
            for (auto& review : reviews[entry.first]) {
                if (review.reviewId != review_id) continue;
                review.reviewReply = gbp::ReviewReply{.comment = entry.second, .updateTime = std::string(reply_time), .reviewReplyState = "APPROVED"};
                review.raw = boost::json::value_from(review);
            }
        }
        held.clear();
    }

    // ---- the read side ----
    std::string account_id = "111";
    std::vector<gbp::Location> locations;
    // location id -> reviews
    std::map<std::string, std::vector<gbp::Review>> reviews;
    std::size_t page_size = 50;
    // When set, the named call fails with this error.
    std::optional<gbp::ApiError> fail_accounts, fail_locations, fail_list, fail_batch, fail_get;
    // Reviews the list endpoints leave out although they exist (Google's list is not always complete).
    std::vector<std::string> hidden_from_lists;
    int list_calls = 0, batch_calls = 0, get_calls = 0;

    void add_location(std::string location_id, std::string title) {
        gbp::Location location;
        location.name = "locations/" + location_id;
        location.title = std::move(title);
        location.raw = boost::json::object{{"name", location.name}, {"title", *location.title}};
        locations.push_back(std::move(location));
    }

    void put(gbp::Review review) {
        const auto name = *gbp::parse_review_name(review.name);
        review.raw = boost::json::value_from(review);
        auto& list = reviews[name.location_id];
        std::erase_if(list, [&](const gbp::Review& r) { return r.reviewId == review.reviewId; });
        list.push_back(std::move(review));
    }

    void remove(const std::string& location_id, const std::string& review_id) {
        std::erase_if(reviews[location_id], [&](const gbp::Review& r) { return r.reviewId == review_id; });
    }

    boost::asio::awaitable<gbp::Result<std::vector<gbp::Account>>> list_accounts() override {
        if (fail_accounts) co_return std::unexpected(*fail_accounts);
        co_return std::vector<gbp::Account>{{.name = "accounts/" + account_id}};
    }

    boost::asio::awaitable<gbp::Result<std::vector<gbp::Location>>> list_locations(std::string) override {
        if (fail_locations) co_return std::unexpected(*fail_locations);
        co_return locations;
    }

    boost::asio::awaitable<gbp::Result<gbp::ReviewsPage>> list_reviews(std::string, std::string location_id,
                                                                      std::optional<std::string> page_token) override {
        ++list_calls;
        if (fail_list) co_return std::unexpected(*fail_list);
        auto page = paged({location_id}, page_token);
        page.total_review_count = static_cast<std::int64_t>(reviews[location_id].size());
        co_return page;
    }

    boost::asio::awaitable<gbp::Result<gbp::ReviewsPage>> batch_get_reviews(std::string, std::vector<std::string> location_ids,
                                                                           std::optional<std::string> page_token) override {
        ++batch_calls;
        last_batch_locations = location_ids;
        if (fail_batch) co_return std::unexpected(*fail_batch);
        co_return paged(location_ids, page_token);
    }

    boost::asio::awaitable<gbp::Result<gbp::Review>> get_review(gbp::ReviewName name) override {
        ++get_calls;
        if (fail_get) co_return std::unexpected(*fail_get);
        for (const auto& review : reviews[name.location_id]) {
            if (review.reviewId == name.review_id) co_return review;
        }
        co_return std::unexpected(gbp::ApiError{gbp::ApiError::Kind::not_found, 404, "HTTP 404 NOT_FOUND"});
    }

    std::vector<std::string> last_batch_locations;

private:
    gbp::ReviewsPage paged(const std::vector<std::string>& location_ids, const std::optional<std::string>& page_token) {
        std::vector<gbp::Review> all;
        for (const auto& id : location_ids) {
            for (const auto& review : reviews[id]) {
                if (!std::ranges::contains(hidden_from_lists, review.reviewId)) all.push_back(review);
            }
        }
        std::ranges::sort(all, std::greater{}, &gbp::Review::updateTime);
        const auto start = page_token ? static_cast<std::size_t>(std::stoul(*page_token)) : 0;
        gbp::ReviewsPage page;
        for (std::size_t i = start; i < std::min(all.size(), start + page_size); ++i) page.reviews.push_back(all[i]);
        if (start + page_size < all.size()) page.next_page_token = std::to_string(start + page_size);
        return page;
    }
};

// What a person writes for an ordinary post: a text and nothing else.
inline domain::LocalPostContent standard(std::string summary = "Fall specials start Monday.") {
    return {.topic_type = "standard",
            .summary = std::move(summary),
            .language_code = "en",
            .call_to_action = std::nullopt,
            .event = std::nullopt,
            .offer = std::nullopt,
            .media_id = std::nullopt,
            .scheduled_time = std::nullopt};
}

// A post's content as a jsonb literal, for a row a test writes straight into the table: as the engine writes it.
inline std::string content_sql(const domain::LocalPostContent& content) {
    std::string literal = "'";
    for (const char c : boost::json::serialize(boost::json::value_from(content))) literal += c == '\'' ? std::string("''") : std::string(1, c);
    return literal + "'::jsonb";
}

// A post as Google would return it.
inline gbp::LocalPost make_local_post(std::string_view account_id, std::string_view location_id, std::string_view post_id,
                                      std::string summary, std::string_view state = "LIVE",
                                      std::string_view created = "2026-11-01T10:00:00Z") {
    gbp::LocalPost post;
    post.name = std::format("accounts/{}/locations/{}/localPosts/{}", account_id, location_id, post_id);
    post.languageCode = "en";
    post.summary = std::move(summary);
    post.createTime = std::string(created);
    post.updateTime = std::string(created);
    post.state = std::string(state);
    post.searchUrl = std::format("https://local.google.com/place?id=1&use=posts&lpsid={}", post_id);
    post.topicType = "STANDARD";
    post.raw = boost::json::value_from(post);
    return post;
}

// The local posts of the same account, held in memory. A create is accepted
// and the post is PROCESSING, as at Google, until finish_processing(): it is
// listed and can be read meanwhile, but is neither scheduled nor shown.
class FakeLocalPosts final : public gbp::LocalPostReader, public gbp::LocalPostWriter {
public:
    std::string account_id = "111";
    // Location id -> its posts at Google, newest first.
    std::map<std::string, std::vector<gbp::LocalPost>> posts;
    int page_size = 100;
    // The time Google stamps a new post with.
    std::string now = "2026-11-10T12:00:01Z";

    // ---- the write side ----
    struct Create {
        std::string location_id;
        boost::json::object body;
    };
    // Every create that reached "Google", refused ones included.
    std::vector<Create> creates;
    // When set, the create is refused with this error and nothing is stored.
    std::optional<gbp::ApiError> fail_create;
    // The connection drops after the request was sent: the caller gets a
    // transport error. With `timeout_after_storing` Google created the post anyway.
    bool create_times_out = false;
    bool timeout_after_storing = false;
    // True: the outbound client would not send the request at all (an address it does not take).
    bool client_refuses = false;
    // Every delete that reached "Google", by the post's name.
    std::vector<std::string> deletes;
    std::optional<gbp::ApiError> fail_delete;
    // The connection drops; with `timeout_after_deleting` the post is gone anyway.
    bool delete_times_out = false;
    bool timeout_after_deleting = false;

    boost::asio::awaitable<gbp::Result<gbp::LocalPost>> create_local_post(std::string account, std::string location_id,
                                                                          boost::json::object body) override {
        creates.push_back({location_id, body});
        if (fail_create) co_return std::unexpected(*fail_create);
        if (create_times_out && !timeout_after_storing) throw boost::system::system_error(boost::asio::error::timed_out);

        boost::json::value payload = body;
        payload.as_object()["name"] = std::format("accounts/{}/locations/{}/localPosts/{}", account, location_id, next_id_++);
        auto post = core::from_json<gbp::LocalPost>(payload).value();
        post.state = "PROCESSING";
        post.createTime = now;
        post.updateTime = now;
        post.searchUrl = "https://local.google.com/place?id=1&use=posts&lpsid=new";
        // Google fetches the file and serves it from an address of its own.
        if (const auto* media = body.if_contains("media"); media && media->is_array()) {
            std::vector<gbp::PostMedia> items;
            for (const auto& item : media->get_array()) {
                items.push_back({.mediaFormat = std::string(item.at("mediaFormat").as_string()),
                                 .googleUrl = std::format("https://lh3.googleusercontent.com/p/fake{}", next_id_)});
            }
            post.media = std::move(items);
        }
        post.raw = boost::json::value_from(post);
        posts[location_id].insert(posts[location_id].begin(), post);
        if (create_times_out) throw boost::system::system_error(boost::asio::error::timed_out);
        co_return post;
    }

    boost::asio::awaitable<gbp::Result<void>> delete_local_post(gbp::LocalPostName name) override {
        if (client_refuses) throw std::invalid_argument("the outbound client only sends DELETE for a local post");
        deletes.push_back(name.str());
        if (fail_delete) co_return std::unexpected(*fail_delete);
        if (delete_times_out && !timeout_after_deleting) throw boost::system::system_error(boost::asio::error::timed_out);
        const auto* post = name.account_id == account_id ? find_post(name.location_id, name.post_id) : nullptr;
        if (!post || !remove(std::string(post->name))) co_return std::unexpected(not_found());
        if (delete_times_out) throw boost::system::system_error(boost::asio::error::timed_out);
        co_return gbp::Result<void>{};
    }

    // ---- what happens at Google ----

    // Google has taken the new posts in: each is scheduled, repeating or live.
    void finish_processing() {
        for (auto& [location, list] : posts) {
            for (auto& post : list) {
                if (post.state != "PROCESSING") continue;
                const bool repeats = post.event && post.event->recurrenceInfo;
                set_state(post, post.scheduledTime ? "SCHEDULED" : repeats ? "RECURRING" : "LIVE");
            }
        }
    }

    // Google refuses the post for its content.
    void reject(std::string_view name) {
        if (auto* post = find(name)) set_state(*post, "REJECTED");
    }

    // A post made in Google's own interface, or changed there.
    void add(const std::string& location_id, gbp::LocalPost post) {
        remove(post.name);
        posts[location_id].insert(posts[location_id].begin(), std::move(post));
    }

    // The post is gone at Google. False if it was not there.
    bool remove(std::string_view name) {
        for (auto& [location, list] : posts) {
            if (std::erase_if(list, [&](const gbp::LocalPost& p) { return p.name == name; }) > 0) return true;
        }
        return false;
    }

    // The post of that location with that id, whatever account its stored name carries: a location
    // that moved to another account keeps its posts.
    gbp::LocalPost* find_post(const std::string& location_id, std::string_view post_id) {
        for (auto& post : posts[location_id]) {
            const auto name = gbp::parse_local_post_name(post.name);
            if (name && name->post_id == post_id) return &post;
        }
        return nullptr;
    }

    // What Google answers for a post that is gone, and for an address that names no location of the account.
    static gbp::ApiError not_found() { return {gbp::ApiError::Kind::not_found, 404, "HTTP 404 NOT_FOUND Requested entity was not found."}; }

    gbp::LocalPost* find(std::string_view name) {
        for (auto& [location, list] : posts) {
            for (auto& post : list) {
                if (post.name == name) return &post;
            }
        }
        return nullptr;
    }

    // ---- the read side ----
    std::optional<gbp::ApiError> fail_list;
    std::optional<gbp::ApiError> fail_get;
    // Posts that exist but appear in no list: only a get finds them.
    std::set<std::string> hidden_from_lists;
    int list_calls = 0;
    int get_calls = 0;

    boost::asio::awaitable<gbp::Result<gbp::LocalPostsPage>> list_local_posts(std::string account, std::string location_id,
                                                                              std::optional<std::string> page_token) override {
        ++list_calls;
        if (fail_list) co_return std::unexpected(*fail_list);
        if (account != account_id) co_return std::unexpected(not_found());
        std::vector<gbp::LocalPost> visible;
        for (const auto& post : posts[location_id]) {
            if (!hidden_from_lists.contains(post.name)) visible.push_back(post);
        }
        const std::size_t start = page_token ? std::stoul(*page_token) : 0;
        gbp::LocalPostsPage page;
        for (std::size_t i = start; i < visible.size() && page.posts.size() < static_cast<std::size_t>(page_size); ++i) {
            page.posts.push_back(visible[i]);
        }
        if (start + page.posts.size() < visible.size()) page.next_page_token = std::to_string(start + page.posts.size());
        co_return page;
    }

    boost::asio::awaitable<gbp::Result<gbp::LocalPost>> get_local_post(gbp::LocalPostName name) override {
        ++get_calls;
        if (client_refuses) throw std::invalid_argument("the outbound client does not send this request");
        if (fail_get) co_return std::unexpected(*fail_get);
        if (name.account_id == account_id) {
            if (const auto* post = find_post(name.location_id, name.post_id)) co_return *post;
        }
        co_return std::unexpected(not_found());
    }

private:
    void set_state(gbp::LocalPost& post, std::string_view state) {
        post.state = std::string(state);
        post.raw = boost::json::value_from(post);
    }

    std::int64_t next_id_ = 7001;
};

class FakeSubscriber final : public pubsub::Subscriber {
public:
    std::deque<pubsub::Message> waiting;
    std::vector<std::string> acknowledged;
    std::optional<pubsub::ApiError> fail_pull;

    void publish(std::string message_id, std::string data) {
        waiting.push_back({.ack_id = "ack-" + message_id,
                           .message_id = std::move(message_id),
                           .publish_time = "2026-11-10T12:00:00Z",
                           .data = std::move(data)});
    }

    boost::asio::awaitable<pubsub::Result<std::vector<pubsub::Message>>> pull(int max_messages) override {
        if (fail_pull) co_return std::unexpected(*fail_pull);
        std::vector<pubsub::Message> out;
        // Like Pub/Sub: a message stays until it is acknowledged.
        for (const auto& message : waiting) {
            if (static_cast<int>(out.size()) == max_messages) break;
            out.push_back(message);
        }
        co_return out;
    }

    boost::asio::awaitable<pubsub::Result<void>> acknowledge(std::vector<std::string> ack_ids) override {
        for (const auto& id : ack_ids) {
            acknowledged.push_back(id);
            std::erase_if(waiting, [&](const pubsub::Message& m) { return m.ack_id == id; });
        }
        co_return pubsub::Result<void>{};
    }
};

// Hands back whichever identity the test registered for a code, and checks
// that the engine sends the PKCE verifier and nonce that match what it put
// in the authorization request.
class FakeIdentityProvider final : public identity::Provider {
public:
    std::map<std::string, identity::Identity> identities;  // by code
    mutable std::optional<identity::AuthorizationRequest> last_request;

    std::string authorization_url(const identity::AuthorizationRequest& request) const override {
        last_request = request;
        return "https://idp.example/authorize?state=" + request.state;
    }

    boost::asio::awaitable<std::expected<identity::Identity, std::string>> redeem(std::string code, std::string code_verifier,
                                                                                  std::string redirect_uri,
                                                                                  std::string nonce) override {
        redeemed_verifier = code_verifier;
        if (!last_request || nonce != last_request->nonce || redirect_uri != last_request->redirect_uri) {
            co_return std::unexpected(std::string("nonce or redirect address doesn't match the authorization request"));
        }
        const auto it = identities.find(code);
        if (it == identities.end()) co_return std::unexpected(std::string("unknown code"));
        co_return it->second;
    }

    std::string redeemed_verifier;
};

// A model whose answers the test scripts. Every request is kept, so a test
// can check exactly what would have been sent to the provider.
class FakeLlm final : public llm::Client {
public:
    using Answer = std::function<llm::Result<llm::ChatResult>(const llm::ChatRequest&)>;

    bool is_configured = true;
    // What the analyzer and the generator answer. The defaults are a model
    // that behaves: happy praise with nothing to flag, and a clean thank-you.
    Answer analyzer = [](const llm::ChatRequest&) { return json_answer(kHappyAnalysis); };
    Answer generator = [](const llm::ChatRequest&) { return json_answer(kCleanReply); };
    // The judge of a go-live test: every reply it is shown is fine, and it
    // rates as many replies as the request holds.
    Answer judge = judging("post_as_is");
    // The summary of a report: one plain paragraph.
    Answer summary = [](const llm::ChatRequest&) { return json_answer(kPlainSummary); };
    // The help with local posts (the task "posts"): ideas, and the text of one post.
    Answer post_ideas = [](const llm::ChatRequest&) { return json_answer(kPostIdeas); };
    Answer post_draft = [](const llm::ChatRequest&) { return json_answer(kPostDraft); };
    std::vector<llm::ChatRequest> requests;

    static constexpr std::string_view kHappyAnalysis = R"({
        "language": "en", "script": "Latn", "sentiment": "positive", "contains_complaint": false,
        "topics": [], "items": [], "severity_flags": [],
        "manipulation_suspected": false, "manipulation_evidence": "", "summary_en": "A happy guest."})";
    // Praise that tries to steer the reply.
    static constexpr std::string_view kSuspiciousAnalysis = R"({
        "language": "en", "script": "Latn", "sentiment": "mixed", "contains_complaint": false,
        "topics": [], "items": [], "severity_flags": [],
        "manipulation_suspected": true, "manipulation_evidence": "it tells the reply what to say", "summary_en": "Tries to steer the reply."})";
    static constexpr std::string_view kCleanReply = R"({
        "reply": "Thank you so much for your kind words! We're glad you enjoyed your visit and hope to welcome you back soon.",
        "language": "en", "script": "Latn", "manipulation_suspected": false, "notes_for_staff": ""})";

    static constexpr std::string_view kPlainSummary =
        R"({"summary": "You had a steady period. Reviewers liked what they found, and your profile was seen often."})";

    static constexpr std::string_view kPostIdeas = R"({"ideas": [
        {"topic_type": "standard", "headline": "The lunch menu", "brief": "Say that the lunch menu is back for the season.",
         "reason": "Many recent reviews praise the food."},
        {"topic_type": "event", "headline": "A tasting evening", "brief": "Invite guests to an evening the business could set up.",
         "reason": "People search for evenings out."},
        {"topic_type": "offer", "headline": "A weekday treat", "brief": "A small offer for weekdays, if the business wants one.",
         "reason": "Weekdays are quieter in the figures."}]})";
    static constexpr std::string_view kPostDraft =
        R"({"summary": "Our guests keep telling us how much they enjoy lunch with us. Come by this week and see what the kitchen has made.",
            "title": "Lunch is served", "notes_for_staff": "Check the opening hours before publishing."})";

    static llm::Result<llm::ChatResult> json_answer(std::string_view json) {
        llm::ChatResult result;
        result.json = boost::json::parse(json).as_object();
        result.prompt_tokens = 1200;
        result.completion_tokens = 80;
        result.finish_reason = "stop";
        return result;
    }
    static llm::Result<llm::ChatResult> failure(llm::Error::Kind kind, std::string message = "scripted failure") {
        return std::unexpected(llm::Error{kind, 0, std::move(message)});
    }
    // How many replies a judge request holds: each is {"reply": n, ...} in its reply_data block.
    static std::size_t replies_in(const llm::ChatRequest& request) {
        std::size_t count = 0;
        const auto& text = request.messages.back().content;
        const auto from = text.find("<reply_data");
        for (auto at = text.find("{\"reply\":", from); at != std::string::npos; at = text.find("{\"reply\":", at + 1)) ++count;
        return count;
    }
    // A judge's answer that gives every one of `count` replies the same rating.
    static std::string ratings(std::size_t count, std::string_view rating) {
        std::string out = R"({"ratings": [)";
        for (std::size_t i = 1; i <= count; ++i) {
            out += std::format(R"({}{{"reply": {}, "rating": "{}", "reason": "scripted"}})", i == 1 ? "" : ", ", i, rating);
        }
        return out + "]}";
    }
    // An answer that always is `json`.
    static Answer always(std::string_view json) {
        return [json = std::string(json)](const llm::ChatRequest&) { return json_answer(json); };
    }
    // An analyzer that is careful: a review that contains `marker` (the client's own) is happy praise, every
    // other one (the corpus) tries to steer the reply.
    static Answer careful(std::string marker) {
        return [marker = std::move(marker)](const llm::ChatRequest& request) {
            const bool own = request.messages.back().content.contains(marker);
            return json_answer(own ? kHappyAnalysis : kSuspiciousAnalysis);
        };
    }
    // A judge that gives every reply it is shown the same rating.
    static Answer judging(std::string rating) {
        return [rating = std::move(rating)](const llm::ChatRequest& request) { return json_answer(ratings(replies_in(request), rating)); };
    }
    // An analyzer's answer with every field; the defaults are a happy guest with nothing to flag.
    struct Analysis {
        std::string sentiment = "positive";
        bool complaint = false;
        bool manipulation = false;
        std::string evidence;
        std::string summary = "A happy guest.";
    };
    static std::string analysis(const Analysis& a) {
        return boost::json::serialize(boost::json::object{
            {"language", "en"}, {"script", "Latn"}, {"sentiment", a.sentiment}, {"contains_complaint", a.complaint},
            {"topics", boost::json::array{}}, {"items", boost::json::array{}}, {"severity_flags", boost::json::array{}},
            {"manipulation_suspected", a.manipulation}, {"manipulation_evidence", a.evidence}, {"summary_en", a.summary}});
    }

    // The providers this model answers for; `is_configured` false means none.
    std::vector<std::string> provider_names{"dashscope", "anthropic"};
    std::vector<std::string> providers() const override { return is_configured ? provider_names : std::vector<std::string>{}; }

    boost::asio::awaitable<llm::Result<llm::ChatResult>> chat_json(llm::ChatRequest request) override {
        requests.push_back(request);
        if (!configured(request.provider)) co_return failure(llm::Error::Kind::not_configured);
        // A named local, so that no temporary is built inside co_return.
        const bool ideas = request.task == "posts" && request.messages.front().content.contains("# Your task: ideas");
        auto result = request.task == "analyzer"  ? analyzer(request)
                      : request.task == "judge"   ? judge(request)
                      : request.task == "summary" ? summary(request)
                      : request.task != "posts"   ? generator(request)
                      : ideas                     ? post_ideas(request)
                                                  : post_draft(request);
        co_return result;
    }

    std::size_t calls(std::string_view task) const {
        return static_cast<std::size_t>(std::ranges::count(requests, task, &llm::ChatRequest::task));
    }
};

// A notification channel that keeps what it was asked to send.
class FakeChannel final : public notify::Channel {
public:
    std::vector<notify::Message> sent;
    // Errors handed out, one per call, before sends succeed.
    std::deque<notify::SendError> failures;

    std::string_view name() const override { return "email"; }
    boost::asio::awaitable<std::expected<void, notify::SendError>> send(notify::Message message) override {
        if (!failures.empty()) {
            auto failure = std::move(failures.front());
            failures.pop_front();
            co_return std::unexpected(std::move(failure));
        }
        sent.push_back(std::move(message));
        co_return std::expected<void, notify::SendError>{};
    }
};

// Apple's push service as the notifier sees it: it keeps every push and
// answers each with the next scripted answer, or `accepted`.
class FakePushSender final : public notify::PushSender {
public:
    std::vector<notify::Push> sent;
    // One per push, in order, before the rest are accepted.
    std::deque<notify::PushAnswer> answers;
    // Errors handed out, one per call, before calls succeed.
    std::deque<notify::SendError> failures;

    boost::asio::awaitable<std::expected<std::vector<notify::PushAnswer>, notify::SendError>> send(std::vector<notify::Push> pushes) override {
        if (!failures.empty()) {
            auto failure = std::move(failures.front());
            failures.pop_front();
            co_return std::unexpected(std::move(failure));
        }
        std::vector<notify::PushAnswer> out;
        for (auto& push : pushes) {
            if (answers.empty()) {
                out.push_back({});
            } else {
                out.push_back(std::move(answers.front()));
                answers.pop_front();
            }
            sent.push_back(std::move(push));
        }
        co_return out;
    }
};

// Google's Performance API, as a report asks it: what each location has,
// and what was asked for.
class FakePerformance final : public gbp::PerformanceReader {
public:
    // By Google location id.
    std::map<std::string, std::vector<domain::MetricDay>> days;
    // By Google location id and "YYYY-MM".
    std::map<std::string, std::map<std::string, std::vector<domain::SearchKeyword>>> keywords;
    // Errors handed out, one per call of daily_metrics, before answers are given.
    std::deque<gbp::ApiError> failures;
    int daily_calls = 0;
    int keyword_calls = 0;

    // One day's value of one metric.
    void set(const std::string& location_id, std::string_view metric, std::chrono::year_month_day day, std::int64_t value) {
        days[location_id].push_back({.metric = std::string(metric), .day = std::chrono::sys_days{day}, .value = value});
    }

    boost::asio::awaitable<gbp::Result<std::vector<domain::MetricDay>>> daily_metrics(std::string location_id, domain::Day from,
                                                                                    domain::Day to) override {
        ++daily_calls;
        if (!failures.empty()) {
            auto failure = std::move(failures.front());
            failures.pop_front();
            co_return std::unexpected(std::move(failure));
        }
        std::vector<domain::MetricDay> out;
        for (const auto& value : days[location_id]) {
            if (value.day >= from && value.day <= to) out.push_back(value);
        }
        co_return out;
    }

    boost::asio::awaitable<gbp::Result<std::vector<domain::SearchKeyword>>> search_keywords(std::string location_id,
                                                                                          std::chrono::year_month month) override {
        ++keyword_calls;
        const auto key = std::format("{:04}-{:02}", static_cast<int>(month.year()), static_cast<unsigned>(month.month()));
        auto out = keywords[location_id][key];
        co_return out;
    }
};

// A bucket held in memory: what was put under which name, the last write winning.
class FakeStore final : public storage::ObjectStore {
public:
    explicit FakeStore(std::string base = "https://storage.test/public/") : base_(std::move(base)) {}

    std::map<std::string, storage::Object> objects;
    // True: every put is refused, as by a bucket the engine may not write to.
    bool refuse = false;
    // True: every put throws, as a connection that drops does.
    bool unreachable = false;
    int puts = 0;

    boost::asio::awaitable<storage::Result<void>> put(storage::Object object) override {
        ++puts;
        if (unreachable) throw std::runtime_error("connection reset");
        if (refuse || !storage::valid_object_name(object.name)) {
            co_return std::unexpected(storage::ApiError{storage::ApiError::Kind::permission_denied, 403, "scripted refusal"});
        }
        auto name = object.name;
        objects.insert_or_assign(std::move(name), std::move(object));
        co_return storage::Result<void>{};
    }
    std::string base_url() const override { return base_; }

private:
    std::string base_;
};

}  // namespace poni::test
