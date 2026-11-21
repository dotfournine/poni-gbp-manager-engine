// JSON to Boost.Describe structs, with errors that name the field.
//
// Boost.JSON's own error for a missing required member is "source composite
// size does not match target size", which says nothing about which member.
// from_json() walks the struct on failure and reports the path, for example
// "missing field 'reviews[3].updateTime'".
//
// Rules the mapping follows (Boost.JSON's): unknown keys are ignored, a
// std::optional member may be absent or null, everything else is required.
// Google omits empty arrays, so list members in Google payloads are
// std::optional<std::vector<T>>.
//
// This is the lenient reader, for what the engine is answered with (Google,
// a model provider, its own stored JSON). A request body the engine is sent
// is read strictly, by core/body.hpp (D-092).
#pragma once

#include <boost/describe/members.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/value.hpp>
#include <boost/json/value_to.hpp>
#include <boost/mp11/algorithm.hpp>

#include <expected>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace poni::core {

namespace json_detail {

template <class T>
struct is_optional : std::false_type {};
template <class T>
struct is_optional<std::optional<T>> : std::true_type {};

template <class T>
struct is_vector : std::false_type {};
template <class T>
struct is_vector<std::vector<T>> : std::true_type {};

template <class T>
using members_of = boost::describe::describe_members<T, boost::describe::mod_public>;

// The sentences both readers refuse with (this one and core/body.hpp's): a member is named by its path.
inline std::string member_path(const std::string& path, std::string_view name) {
    return path.empty() ? std::string(name) : std::format("{}.{}", path, name);
}
inline std::string missing(const std::string& path) { return std::format("missing field '{}'", path); }
inline std::string not_a_list(const std::string& path) { return std::format("field '{}' must be a list", path); }
inline std::string not_an_object(const std::string& path) { return std::format("field '{}' must be an object", path); }
inline std::string wrong_type(const std::string& path) { return std::format("field '{}' has the wrong type", path); }

// Why `v` doesn't convert to T, or an empty string if this walk finds nothing.
template <class T>
std::string diagnose(const boost::json::value& v, const std::string& path) {
    if constexpr (is_optional<T>::value) {
        return v.is_null() ? std::string{} : diagnose<typename T::value_type>(v, path);
    } else if constexpr (is_vector<T>::value) {
        if (!v.is_array()) return not_a_list(path);
        std::size_t i = 0;
        for (const auto& element : v.get_array()) {
            if (auto why = diagnose<typename T::value_type>(element, std::format("{}[{}]", path, i++)); !why.empty()) {
                return why;
            }
        }
        return {};
    } else if constexpr (boost::describe::has_describe_members<T>::value) {
        if (!v.is_object()) {
            return path.empty() ? std::string("expected a JSON object") : not_an_object(path);
        }
        const auto& object = v.get_object();
        std::string why;
        boost::mp11::mp_for_each<members_of<T>>([&](auto member) {
            if (!why.empty()) return;
            using M = std::remove_cvref_t<decltype(std::declval<T&>().*member.pointer)>;
            const std::string sub = member_path(path, member.name);
            const auto* value = object.if_contains(member.name);
            if (!value) {
                if (!is_optional<M>::value) why = missing(sub);
                return;
            }
            why = diagnose<M>(*value, sub);
        });
        return why;
    } else {
        if (boost::json::try_value_to<T>(v)) return {};
        return path.empty() ? std::string("the value has the wrong type") : wrong_type(path);
    }
}

}  // namespace json_detail

template <class T>
std::expected<T, std::string> from_json(const boost::json::value& v) {
    auto result = boost::json::try_value_to<T>(v);
    if (result) return std::move(*result);
    auto why = json_detail::diagnose<T>(v, {});
    return std::unexpected(why.empty() ? result.error().message() : std::move(why));
}

template <class T>
std::expected<T, std::string> parse_json(std::string_view text) {
    boost::system::error_code ec;
    const auto value = boost::json::parse(text, ec);
    if (ec) return std::unexpected(std::format("not valid JSON: {}", ec.message()));
    return from_json<T>(value);
}

}  // namespace poni::core
