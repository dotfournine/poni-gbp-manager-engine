#include "domain/report_html.hpp"

#include "core/text.hpp"

#include <algorithm>
#include <cmath>
#include <format>

namespace poni::domain {

namespace {

// Everything a browser needs to know before it reads a byte of untrusted
// text: no script, no frames, no forms, nothing loaded but inline styles
// and reviewers' photos.
constexpr std::string_view kHead = R"(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<meta name="robots" content="noindex, nofollow, noarchive">
<meta name="referrer" content="no-referrer">
<meta http-equiv="Content-Security-Policy" content="default-src 'none'; style-src 'unsafe-inline'; img-src https://*.googleusercontent.com data:; base-uri 'none'; form-action 'none'">
)";

constexpr std::string_view kStyle = R"(<style>
:root { --ink:#1c2430; --muted:#5d6878; --line:#dde2ea; --panel:#f5f7fa; --accent:#1f5fbf; --good:#2e8b57; --mixed:#d9a520;
        --neutral:#9aa5b5; --bad:#c8453b; --star:#e8a317; }
* { box-sizing: border-box; }
body { margin:0; background:#eef1f5; color:var(--ink); font:16px/1.5 -apple-system, "Segoe UI", Roboto, "Helvetica Neue", Arial,
       "PingFang SC", "Microsoft YaHei", sans-serif; }
main { max-width:920px; margin:0 auto; padding:24px 16px 48px; background:#fff; }
header { border-bottom:3px solid var(--accent); padding-bottom:16px; margin-bottom:24px; }
h1 { font-size:28px; line-height:1.2; margin:0 0 4px; }
h2 { font-size:19px; margin:32px 0 12px; padding-bottom:6px; border-bottom:1px solid var(--line); }
h3 { font-size:15px; margin:0 0 8px; color:var(--muted); font-weight:600; }
p { margin:0 0 12px; }
.sub { color:var(--muted); margin:0; }
.summary { background:var(--panel); border-left:4px solid var(--accent); padding:14px 16px; white-space:pre-line; }
.tiles { display:grid; grid-template-columns:repeat(auto-fit, minmax(150px, 1fr)); gap:12px; }
.tile { border:1px solid var(--line); border-radius:8px; padding:12px 14px; }
.tile .label { color:var(--muted); font-size:13px; }
.tile .value { font-size:26px; font-weight:650; line-height:1.25; }
.tile .note { color:var(--muted); font-size:13px; }
.up { color:var(--good); } .down { color:var(--bad); }
.cols { display:grid; grid-template-columns:repeat(auto-fit, minmax(280px, 1fr)); gap:24px; }
svg { max-width:100%; height:auto; display:block; }
table { border-collapse:collapse; width:100%; font-size:14px; }
th, td { text-align:left; padding:6px 8px; border-bottom:1px solid var(--line); vertical-align:top; }
th.num, td.num { text-align:right; font-variant-numeric:tabular-nums; white-space:nowrap; }
th { color:var(--muted); font-weight:600; }
.scroll { overflow-x:auto; }
.rank { list-style:none; margin:0; padding:0; }
.rank li { display:grid; grid-template-columns:1fr auto; gap:8px; padding:4px 0; }
.rank .bar { grid-column:1 / -1; height:6px; border-radius:3px; background:var(--panel); }
.rank .bar i { display:block; height:100%; border-radius:3px; }
.review { border:1px solid var(--line); border-radius:8px; padding:12px 14px; margin-bottom:12px; break-inside:avoid; }
.review .who { display:flex; align-items:center; gap:10px; margin-bottom:6px; }
.review img, .review .avatar { width:36px; height:36px; border-radius:50%; flex:none; }
.review .avatar { background:var(--panel); color:var(--muted); display:flex; align-items:center; justify-content:center; font-weight:600; }
.review .name { font-weight:600; overflow-wrap:anywhere; }
.review .meta { color:var(--muted); font-size:13px; }
.review .text { white-space:pre-line; overflow-wrap:anywhere; margin:0; }
.stars { color:var(--star); letter-spacing:1px; }
.stars .off { color:var(--line); }
.legend { color:var(--muted); font-size:13px; margin-top:6px; }
.legend i { display:inline-block; width:10px; height:10px; border-radius:2px; margin:0 4px 0 10px; }
.empty { color:var(--muted); font-style:italic; }
.staff-list { margin-bottom:16px; }
table.staff td.name { overflow-wrap:break-word; min-width:5em; }
table.staff td.where { color:var(--muted); }
footer { margin-top:40px; padding-top:12px; border-top:1px solid var(--line); color:var(--muted); font-size:13px; }
@media (max-width:520px) { h1 { font-size:23px; } .tile .value { font-size:22px; } }
@media print {
  @page { margin:14mm; }
  body { background:#fff; font-size:11pt; }
  main { max-width:none; padding:0 1px; }
  h2, h3 { break-after:avoid; }
  .tile, .review, table, svg, .rank li { break-inside:avoid; }
  * { -webkit-print-color-adjust:exact; print-color-adjust:exact; }
}
</style>
)";

// 12345 -> "12,345".
std::string number(std::int64_t n) {
    auto digits = std::to_string(n < 0 ? -n : n);
    for (auto at = static_cast<std::ptrdiff_t>(digits.size()) - 3; at > 0; at -= 3) digits.insert(static_cast<std::size_t>(at), ",");
    return n < 0 ? "-" + digits : digits;
}

std::string one_decimal(double v) { return std::format("{:.1f}", v); }

std::string rating(const std::optional<double>& v) { return v ? std::format("{:.2f}", *v) : std::string("–"); }

// A share as a whole percent: 0.456 -> "46%".
std::string as_percent(const std::optional<double>& share) { return share ? std::format("{}%", std::lround(*share * 100)) : std::string("–"); }

// A change in percent with its sign: "+12%", "-8%", "0%".
std::string signed_percent(double change) { return std::format("{}{}%", change > 0 ? "+" : "", std::lround(change)); }

std::string hours(const std::optional<double>& v) {
    if (!v) return "–";
    if (*v < 1) return std::format("{} min", std::max<long>(std::lround(*v * 60), 1));
    if (*v < 48) return std::format("{} h", one_decimal(*v));
    return std::format("{} days", one_decimal(*v / 24));
}

// "+12%" in green or "-8%" in red, against the period before; "" when there is nothing to compare with.
std::string change(double now, double before, bool has_before, bool higher_is_better = true) {
    if (!has_before) return {};
    const auto percent = percent_change(now, before);
    if (!percent) return now == before ? std::string("no change") : std::string("none in the period before");
    const auto rounded = std::lround(*percent);
    if (rounded == 0) return "no change";
    const bool good = (rounded > 0) == higher_is_better;
    return std::format("<span class=\"{}\">{}</span> vs. the period before", good ? "up" : "down", signed_percent(*percent));
}

// A change of an average rating, in stars.
std::string rating_change(const std::optional<double>& now, const std::optional<double>& before) {
    if (!now || !before) return {};
    const auto diff = *now - *before;
    if (std::abs(diff) < 0.005) return "no change";
    return std::format("<span class=\"{}\">{}{:.2f}</span> vs. the period before", diff > 0 ? "up" : "down", diff > 0 ? "+" : "", diff);
}

void tile(std::string& out, std::string_view label, std::string_view value, std::string_view note_html) {
    out += std::format("<div class=\"tile\"><div class=\"label\">{}</div><div class=\"value\">{}</div><div class=\"note\">{}</div></div>\n",
                       html_escape(label), html_escape(value), note_html);
}

std::string stars_html(int stars) {
    std::string out = "<span class=\"stars\" aria-label=\"" + std::to_string(stars) + " of 5 stars\">";
    for (int i = 1; i <= 5; ++i) out += i <= stars ? "★" : "<span class=\"off\">★</span>";
    return out + "</span>";
}

// ---- charts -----------------------------------------------------------------------

std::string star_chart(const ReviewNumbers& numbers) {
    const auto most = std::max<std::int64_t>(*std::ranges::max_element(numbers.stars), 1);
    std::string out = "<svg viewBox=\"0 0 360 150\" role=\"img\" aria-label=\"Reviews by star rating\">";
    for (int stars = 5; stars >= 1; --stars) {
        const auto count = numbers.stars[static_cast<std::size_t>(stars - 1)];
        const auto y = (5 - stars) * 28 + 8;
        const auto width = static_cast<double>(count) / static_cast<double>(most) * 250.0;
        out += std::format("<text x=\"0\" y=\"{}\" font-size=\"13\" fill=\"#5d6878\">{} ★</text>", y + 14, stars);
        out += std::format("<rect x=\"36\" y=\"{}\" width=\"250\" height=\"18\" rx=\"3\" fill=\"#f5f7fa\"/>", y);
        out += std::format("<rect x=\"36\" y=\"{}\" width=\"{:.1f}\" height=\"18\" rx=\"3\" fill=\"#e8a317\"/>", y, width);
        out += std::format("<text x=\"294\" y=\"{}\" font-size=\"13\" fill=\"#1c2430\">{}</text>", y + 14, number(count));
    }
    return out + "</svg>";
}

struct Segment {
    std::int64_t value;
    std::string_view color;
};

struct Bar {
    std::string label;
    std::vector<Segment> segments;
};

// One bar per bucket, stacked. Labels under the first, the last and a few between.
std::string bar_chart(const std::vector<Bar>& bars, std::string_view title) {
    const double width = 640, height = 190, left = 34, bottom = 24, top = 8;
    std::int64_t most = 1;
    for (const auto& bar : bars) {
        std::int64_t sum = 0;
        for (const auto& segment : bar.segments) sum += segment.value;
        most = std::max(most, sum);
    }
    const auto count = std::max<std::size_t>(bars.size(), 1);
    const double slot = (width - left) / static_cast<double>(count);
    const double bar_width = std::max(slot * 0.7, 1.0);
    const double plot = height - bottom - top;
    std::string out = std::format("<svg viewBox=\"0 0 {} {}\" role=\"img\" aria-label=\"{}\">", width, height, html_escape(title));
    out += std::format("<line x1=\"{0}\" y1=\"{1}\" x2=\"{2}\" y2=\"{1}\" stroke=\"#dde2ea\"/>", left, height - bottom, width);
    out += std::format("<text x=\"{}\" y=\"{}\" font-size=\"11\" fill=\"#5d6878\" text-anchor=\"end\">{}</text>", left - 4, top + 10,
                       number(most));
    out += std::format("<text x=\"{}\" y=\"{}\" font-size=\"11\" fill=\"#5d6878\" text-anchor=\"end\">0</text>", left - 4, height - bottom);
    const auto label_every = std::max<std::size_t>((count + 6) / 7, 1);
    for (std::size_t i = 0; i < bars.size(); ++i) {
        const double x = left + slot * static_cast<double>(i) + (slot - bar_width) / 2;
        double y = height - bottom;
        for (const auto& segment : bars[i].segments) {
            if (segment.value <= 0) continue;
            const double h = static_cast<double>(segment.value) / static_cast<double>(most) * plot;
            y -= h;
            out += std::format("<rect x=\"{:.1f}\" y=\"{:.1f}\" width=\"{:.1f}\" height=\"{:.1f}\" fill=\"{}\"/>", x, y, bar_width, h,
                               segment.color);
        }
        // The last bar is labelled too, unless that would run into the label before it.
        const bool last = i + 1 == bars.size() && (i % label_every) * 2 >= label_every;
        if (i % label_every == 0 || last) {
            // The last bar touches the edge of the chart: its label ends with it instead of being centered and cut off.
            const bool at_edge = i + 1 == bars.size() && bars.size() > 1;
            out += std::format("<text x=\"{:.1f}\" y=\"{}\" font-size=\"11\" fill=\"#5d6878\" text-anchor=\"{}\">{}</text>",
                               at_edge ? x + bar_width : x + bar_width / 2, height - 8, at_edge ? "end" : "middle",
                               html_escape(bars[i].label));
        }
    }
    return out + "</svg>";
}

void ranked(std::string& out, std::string_view title, const std::vector<Ranked>& entries, std::string_view color) {
    out += std::format("<div><h3>{}</h3>", html_escape(title));
    if (entries.empty()) {
        out += "<p class=\"empty\">Nothing stood out in this period.</p></div>\n";
        return;
    }
    const auto most = std::max<std::int64_t>(entries.front().count, 1);
    out += "<ul class=\"rank\">";
    for (const auto& entry : entries) {
        out += std::format("<li><span>{}</span><span>{}</span><span class=\"bar\"><i style=\"width:{:.0f}%;background:{}\"></i></span></li>",
                           html_escape(entry.label), number(entry.count),
                           static_cast<double>(entry.count) / static_cast<double>(most) * 100.0, color);
    }
    out += "</ul></div>\n";
}

// One list of the employee recognition section: counts only, no review text.
void staff_table(std::string& out, std::string_view title, const std::vector<StaffMention>& people, bool with_location,
                 std::string_view none) {
    out += std::format("<div class=\"staff-list\"><h3>{}</h3>", html_escape(title));
    if (people.empty()) {
        out += std::format("<p class=\"empty\">{}</p></div>\n", html_escape(none));
        return;
    }
    out += "<div class=\"scroll\"><table class=\"staff\"><tr><th class=\"num\">#</th><th>Name</th>";
    if (with_location) out += "<th>Location</th>";
    out += "<th class=\"num\">Praised</th><th class=\"num\">Criticized</th></tr>";
    for (const auto& person : people) {
        out += std::format("<tr><td class=\"num\">{}</td><td class=\"name\">{}</td>", person.rank, html_escape(person.name));
        if (with_location) out += std::format("<td class=\"where\">{}</td>", html_escape(person.location_title));
        out += std::format("<td class=\"num\">{}</td><td class=\"num\">{}</td></tr>", number(person.positive), number(person.negative));
    }
    out += "</table></div></div>\n";
}

void review_cards(std::string& out, const std::vector<Highlight>& reviews) {
    for (const auto& review : reviews) {
        out += "<div class=\"review\"><div class=\"who\">";
        const auto name = std::string(text::trim(review.reviewer_name));
        if (const auto photo = review.reviewer_photo_url ? safe_photo_url(*review.reviewer_photo_url) : std::nullopt) {
            out += std::format("<img src=\"{}\" alt=\"\" width=\"36\" height=\"36\" loading=\"lazy\" referrerpolicy=\"no-referrer\">",
                               html_escape(*photo));
        } else {
            const auto initial = text::decode(name);
            out += std::format("<span class=\"avatar\">{}</span>",
                               html_escape(initial.empty() ? std::string("?") : text::encode(initial.substr(0, 1))));
        }
        out += std::format("<div><div class=\"name\">{}</div><div class=\"meta\">{} · {}{}</div></div></div>",
                           html_escape(name.empty() ? std::string("A Google user") : name), stars_html(review.rating),
                           html_escape(day_label(review.day)),
                           review.location_title.empty() ? std::string{} : " · " + html_escape(review.location_title));
        out += std::format("<p class=\"text\">{}</p></div>\n", html_escape(review.text));
    }
}

void visibility_row(std::string& out, std::string_view label, std::int64_t now, const std::optional<std::int64_t>& before) {
    out += std::format("<tr><td>{}</td><td class=\"num\">{}</td>", html_escape(label), number(now));
    if (before) {
        const auto percent = percent_change(static_cast<double>(now), static_cast<double>(*before));
        out += std::format("<td class=\"num\">{}</td><td class=\"num\">{}</td>", number(*before),
                           percent ? signed_percent(*percent) : std::string("–"));
    }
    out += "</tr>";
}

}  // namespace

std::string html_escape(std::string_view text) {
    std::string out;
    out.reserve(text.size() + text.size() / 8);
    for (const char c : text) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            case '`': out += "&#96;"; break;
            default:
                // Control characters have no business in a page; a tab and a line break stay.
                if (static_cast<unsigned char>(c) < 0x20 && c != '\n' && c != '\t') break;
                out += c;
        }
    }
    return out;
}

std::optional<std::string> safe_photo_url(std::string_view url) {
    constexpr std::string_view scheme = "https://";
    if (!url.starts_with(scheme) || url.size() > 500) return std::nullopt;
    const auto rest = url.substr(scheme.size());
    // The host ends where the path, a query or a fragment begins: "evil.example?x.googleusercontent.com" is not Google's.
    const auto host = rest.substr(0, rest.find_first_of("/?#"));
    if (host != "googleusercontent.com" && !host.ends_with(".googleusercontent.com")) return std::nullopt;
    for (const char c : url) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                        std::string_view("-._~:/?=&%+").contains(c);
        if (!ok) return std::nullopt;
    }
    // The host must be the whole authority: no user name, no port.
    if (host.contains('@') || host.contains(':')) return std::nullopt;
    return std::string(url);
}

std::string render_report(const ReportData& data) {
    const auto& now = data.reviews;
    const auto& before = data.previous;
    const auto period = period_label(data.period_start, data.period_end);
    std::string out(kHead);
    out += std::format("<title>{} · {}</title>\n", html_escape(data.client_name), html_escape(period));
    out += kStyle;
    out += "</head>\n<body>\n<main>\n";
    out += std::format("<header><h1>{}</h1><p class=\"sub\">Google reviews and visibility · {}</p></header>\n",
                       html_escape(data.client_name), html_escape(period));

    if (data.summary) {
        out += std::format("<section><p class=\"summary\">{}</p></section>\n", html_escape(*data.summary));
    }

    // ---- headline numbers ----
    out += "<section><h2>At a glance</h2><div class=\"tiles\">\n";
    tile(out, "New reviews", number(now.new_reviews),
         change(static_cast<double>(now.new_reviews), static_cast<double>(before.new_reviews), true));
    tile(out, "Average rating, this period", rating(now.average_rating), rating_change(now.average_rating, before.average_rating));
    tile(out, "Overall rating", rating(data.running_average), std::format("{} reviews in total", number(data.running_count)));
    tile(out, "Reviews answered", as_percent(now.response_rate), std::format("{} of {}", number(now.replied), number(now.new_reviews)));
    tile(out, "Typical time to answer", hours(now.median_response_hours), "median, for the reviews answered");
    if (data.visibility.available) {
        const auto views = data.visibility.totals.views();
        tile(out, "Profile views", number(views),
             data.visibility.previous
                 ? change(static_cast<double>(views), static_cast<double>(data.visibility.previous->views()), true)
                 : std::format("through {}", html_escape(short_day(*data.visibility.as_of))));
    }
    out += "</div></section>\n";

    // ---- reviews ----
    out += "<section><h2>Reviews</h2>\n";
    if (now.new_reviews == 0) {
        out += "<p class=\"empty\">No new reviews in this period.</p>\n";
    } else {
        out += "<div class=\"cols\"><div><h3>By star rating</h3>" + star_chart(now) + "</div><div><h3>How reviewers felt</h3>";
        const auto analyzed = now.positive + now.mixed + now.neutral + now.negative;
        if (analyzed == 0) {
            out += "<p class=\"empty\">The reviews of this period have not been analyzed.</p>";
        } else {
            out += "<table><tr><th></th><th class=\"num\">This period</th><th class=\"num\">Period before</th></tr>";
            const auto row = [&](std::string_view label, std::int64_t a, std::int64_t b) {
                out += std::format("<tr><td>{}</td><td class=\"num\">{}</td><td class=\"num\">{}</td></tr>", label, number(a), number(b));
            };
            row("Positive", now.positive, before.positive);
            row("Mixed", now.mixed, before.mixed);
            row("Neutral", now.neutral, before.neutral);
            row("Negative", now.negative, before.negative);
            out += "</table>";
        }
        out += "</div></div>\n";
        if (data.trend.size() > 1) {
            std::vector<Bar> bars;
            for (const auto& bucket : data.trend) {
                const auto unrated = bucket.new_reviews - bucket.positive - bucket.mixed - bucket.neutral - bucket.negative;
                bars.push_back({short_day(bucket.start),
                                {{bucket.negative, "#c8453b"},
                                 {bucket.mixed, "#d9a520"},
                                 {bucket.neutral, "#9aa5b5"},
                                 {bucket.positive, "#2e8b57"},
                                 {std::max<std::int64_t>(unrated, 0), "#dde2ea"}}});
            }
            out += "<h3 style=\"margin-top:16px\">New reviews over the period</h3>";
            out += bar_chart(bars, "New reviews over the period, by how reviewers felt");
            out += "<p class=\"legend\"><i style=\"background:#2e8b57;margin-left:0\"></i>Positive<i style=\"background:#9aa5b5\"></i>Neutral"
                   "<i style=\"background:#d9a520\"></i>Mixed<i style=\"background:#c8453b\"></i>Negative</p>\n";
        }
    }
    if (data.escalations_opened > 0 || data.escalations_closed > 0) {
        out += std::format("<p style=\"margin-top:12px\">Serious issues raised by reviewers: {} opened, {} closed.</p>\n",
                           number(data.escalations_opened), number(data.escalations_closed));
    }
    out += "</section>\n";

    // ---- what people talk about ----
    if (now.new_reviews > 0) {
        out += "<section><h2>What reviewers talk about</h2><div class=\"cols\">\n";
        ranked(out, "Praised most", data.topics_praised, "#2e8b57");
        ranked(out, "Complained about most", data.topics_complained, "#c8453b");
        ranked(out, "Items praised", data.items_praised, "#2e8b57");
        ranked(out, "Items complained about", data.items_complained, "#c8453b");
        out += "</div></section>\n";
    }

    // ---- employee recognition (D-086) ----
    if (data.staff) {
        const auto& staff = *data.staff;
        out += "<section><h2>Employee recognition</h2>\n";
        if (staff.reviews_naming == 0) {
            out += "<p class=\"empty\">No reviewer named a staff member in this period.</p>\n";
        } else {
            const bool with_location = data.locations.size() > 1;
            staff_table(out, "Recognized by customers", staff.recognized, with_location, "Nobody was named in praise in this period.");
            staff_table(out, "Named in complaints", staff.complained, with_location, "Nobody was named in a complaint in this period.");
            out += std::format("<p class=\"sub\">Based on {} {} that named a staff member; each number is a count "
                               "of reviews. Names are as reviewers wrote them, so one person may appear under two spellings.</p>\n",
                               number(staff.reviews_naming), staff.reviews_naming == 1 ? "review" : "reviews");
        }
        out += "</section>\n";
    }

    if (!data.highlights_best.empty() || !data.highlights_worst.empty()) {
        out += "<section><h2>Reviews worth reading</h2>\n";
        if (!data.highlights_best.empty()) {
            out += "<h3>The best</h3>\n";
            review_cards(out, data.highlights_best);
        }
        if (!data.highlights_worst.empty()) {
            out += "<h3>The most critical</h3>\n";
            review_cards(out, data.highlights_worst);
        }
        out += "</section>\n";
    }

    // ---- visibility ----
    out += "<section><h2>Visibility on Google</h2>\n";
    const auto& visibility = data.visibility;
    if (!visibility.available) {
        out += std::format("<p class=\"empty\">{}</p>\n", html_escape(visibility.note));
    } else {
        if (!visibility.complete) {
            out += std::format("<p class=\"sub\">Google publishes these numbers a few days late: they go up to {}.</p>\n",
                               html_escape(day_label(*visibility.as_of)));
        }
        if (!visibility.note.empty()) out += std::format("<p class=\"sub\">{}</p>\n", html_escape(visibility.note));
        if (visibility.daily.size() > 1) {
            std::vector<Bar> bars;
            for (const auto& day : visibility.daily) bars.push_back({short_day(day.day), {{day.views, "#1f5fbf"}}});
            out += "<h3 style=\"margin-top:12px\">Profile views per day</h3>" + bar_chart(bars, "Profile views per day");
        }
        const auto& t = visibility.totals;
        const auto& p = visibility.previous;
        const auto prior = [&](std::int64_t VisibilityTotals::* member) {
            return p ? std::optional<std::int64_t>((*p).*member) : std::optional<std::int64_t>{};
        };
        out += "<div class=\"scroll\"><table style=\"margin-top:12px\"><tr><th></th><th class=\"num\">This period</th>";
        if (p) out += "<th class=\"num\">Period before</th><th class=\"num\">Change</th>";
        out += "</tr>";
        visibility_row(out, "Views on Search, mobile", t.search_mobile, prior(&VisibilityTotals::search_mobile));
        visibility_row(out, "Views on Search, desktop", t.search_desktop, prior(&VisibilityTotals::search_desktop));
        visibility_row(out, "Views on Maps, mobile", t.maps_mobile, prior(&VisibilityTotals::maps_mobile));
        visibility_row(out, "Views on Maps, desktop", t.maps_desktop, prior(&VisibilityTotals::maps_desktop));
        visibility_row(out, "Calls", t.calls, prior(&VisibilityTotals::calls));
        visibility_row(out, "Direction requests", t.direction_requests, prior(&VisibilityTotals::direction_requests));
        visibility_row(out, "Website clicks", t.website_clicks, prior(&VisibilityTotals::website_clicks));
        visibility_row(out, "Menu clicks", t.menu_clicks, prior(&VisibilityTotals::menu_clicks));
        visibility_row(out, "Bookings", t.bookings, prior(&VisibilityTotals::bookings));
        out += "</table></div>\n";
    }
    if (data.keywords && !data.keywords->items.empty()) {
        out += std::format("<h3 style=\"margin-top:20px\">What people searched for, {}</h3>", month_label(data.keywords->month));
        out += "<table><tr><th>Search</th><th class=\"num\">Times shown</th></tr>";
        for (const auto& keyword : data.keywords->items) {
            out += std::format("<tr><td>{}</td><td class=\"num\">{}</td></tr>", html_escape(keyword.keyword),
                               keyword.impressions ? number(*keyword.impressions)
                                                   : std::format("fewer than {}", number(keyword.below.value_or(0))));
        }
        out += "</table>\n";
    }
    out += "</section>\n";

    // ---- per location ----
    if (data.locations.size() > 1) {
        out += "<section><h2>By location</h2><div class=\"scroll\"><table><tr><th>Location</th><th class=\"num\">New reviews</th>"
               "<th class=\"num\">Average</th><th class=\"num\">Overall</th><th class=\"num\">Answered</th><th class=\"num\">Views</th>"
               "<th class=\"num\">Calls</th><th class=\"num\">Directions</th></tr>";
        for (const auto& location : data.locations) {
            const auto& v = location.visibility;
            out += std::format("<tr><td>{}</td><td class=\"num\">{}</td><td class=\"num\">{}</td><td class=\"num\">{} ({})</td>"
                               "<td class=\"num\">{}</td><td class=\"num\">{}</td><td class=\"num\">{}</td><td class=\"num\">{}</td></tr>",
                               html_escape(location.title), number(location.reviews.new_reviews), rating(location.reviews.average_rating),
                               rating(location.running_average), number(location.running_count),
                               as_percent(location.reviews.response_rate),
                               v ? number(v->views()) : std::string("–"), v ? number(v->calls) : std::string("–"),
                               v ? number(v->direction_requests) : std::string("–"));
        }
        out += "</table></div></section>\n";
    }

    out += std::format("<footer>Period: {} ({}). Compared with {}. Prepared by PONI on {}.</footer>\n", html_escape(period),
                       html_escape(data.time_zone), html_escape(period_label(data.previous_start, data.previous_end)),
                       html_escape(day_label(std::string_view(data.generated_at).substr(0, 10))));
    out += "</main>\n</body>\n</html>\n";
    return out;
}

std::string render_revoked_page() {
    std::string out(kHead);
    out += "<title>Report not available</title>\n";
    out += kStyle;
    out += "</head>\n<body>\n<main>\n<header><h1>This report is no longer available</h1></header>\n"
           "<p>The link you followed has been withdrawn. Please ask your contact at PONI for a new one.</p>\n"
           "</main>\n</body>\n</html>\n";
    return out;
}

std::string render_report_email(const ReportData& data, std::string_view link) {
    const auto& now = data.reviews;
    std::string out = std::format("Your Google reviews and visibility report for {} is ready.\n\n", period_label(data.period_start, data.period_end));
    out += std::format("New reviews: {}\n", number(now.new_reviews));
    if (now.average_rating) out += std::format("Average rating in the period: {}\n", rating(now.average_rating));
    if (data.running_average) out += std::format("Overall rating: {} ({} reviews)\n", rating(data.running_average), number(data.running_count));
    if (now.response_rate) out += std::format("Reviews answered: {}\n", as_percent(now.response_rate));
    if (data.visibility.available) {
        out += std::format("Profile views: {} (through {})\n", number(data.visibility.totals.views()), day_label(*data.visibility.as_of));
        out += std::format("Calls: {}, direction requests: {}, website clicks: {}\n", number(data.visibility.totals.calls),
                           number(data.visibility.totals.direction_requests), number(data.visibility.totals.website_clicks));
    }
    if (!data.topics_praised.empty()) out += std::format("Praised most: {}\n", data.topics_praised.front().label);
    if (!data.topics_complained.empty()) out += std::format("Complained about most: {}\n", data.topics_complained.front().label);
    out += std::format("\nThe full report, with charts:\n{}\n\nTo keep a copy, open the report and print it to PDF.\n", link);
    return out;
}

}  // namespace poni::domain
