// A report as one self-contained HTML document (D-085): inline CSS with a
// print stylesheet, charts as inline SVG, no script and nothing loaded from
// elsewhere except reviewers' photos from Google's image hosts. The page is
// rendered once and put into the public bucket, so a report never changes.
//
// Review text, reviewer names, item names and the model's summary are
// untrusted. Nothing reaches the document except through escape(), and a
// photo's address only after safe_photo_url() accepted it.
//
// Cloud Storage serves the page and can send neither a noindex nor a
// content-security-policy header, so both are <meta> elements here.
#pragma once

#include "domain/report.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace poni::domain {

// Text for element content and for a double-quoted attribute value.
std::string html_escape(std::string_view text);

// The address if it is an https address on a googleusercontent.com host
// made of harmless characters; nothing otherwise.
std::optional<std::string> safe_photo_url(std::string_view url);

std::string render_report(const ReportData& data);

// The page's format: its object's extension and the type it is served with. A later PDF changes these two and nothing else.
inline constexpr std::string_view kReportPageExtension = "html";
inline constexpr std::string_view kReportPageContentType = "text/html; charset=utf-8";

// The plain-text email that goes with a report: headline numbers and the link.
std::string render_report_email(const ReportData& data, std::string_view link);

}  // namespace poni::domain
