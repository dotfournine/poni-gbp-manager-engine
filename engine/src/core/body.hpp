// A request body, read as api/openapi.yaml describes it (D-092). This is the
// strict reader for what the engine is sent; json.hpp's lenient one keeps
// reading what Google and the models answer, where the engine is the client
// and ignores what it does not know.
//
// A Boost.Describe struct says what its schema says through its members' types:
//
//   T                  required, never null
//   std::optional<T>   may be left out, never null
//   Nullable<T>        may be left out and may be null: the three states of a
//                      PATCH member that can be cleared (RFC 7396)
//
// A struct that declares `static constexpr bool sent_whole = true;` is one
// that always travels whole, in the shape the engine itself writes it: every
// member must be there, and a std::optional member is the one that may be
// null. Each struct says it for itself, also where it is inside another.
//
// Everything else is refused with a sentence that names the member: one the
// struct does not have, a null where none is allowed, a member that is
// missing, a value of the wrong type. A whole number may be written with a
// fraction of zero (100.0), as JSON Schema counts it. Lengths, ranges and
// formats are not checked here: they belong to whoever takes the value.
#pragma once

#include "core/json.hpp"
#include "core/text.hpp"

#include <boost/describe/members.hpp>
#include <boost/json/object.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/value.hpp>
#include <boost/json/value_to.hpp>
#include <boost/mp11/algorithm.hpp>

#include <concepts>
#include <expected>
#include <format>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace poni::core {

// A member of a body that was left out, is null, or has a value. In a PATCH:
// left out keeps what is stored, null clears it, a value replaces it.
// Default-constructed it is left out; from std::nullopt it is null.
template <class T>
class Nullable {
public:
    using value_type = T;

    Nullable() = default;
    Nullable(std::nullopt_t) : given_(true) {}
    Nullable(std::optional<T> value) : given_(true), value_(std::move(value)) {}
    template <class U>
        requires std::convertible_to<U, T>
    Nullable(U&& value) : given_(true), value_(std::in_place, std::forward<U>(value)) {}

    // Null or a value: the body has the member.
    bool given() const { return given_; }
    // The value; nothing for null, and for a member that was left out.
    const std::optional<T>& value() const { return value_; }
    // What `stored` becomes under this member of a PATCH.
    std::optional<T> over(std::optional<T> stored) const { return given_ ? value_ : std::move(stored); }

    bool operator==(const Nullable&) const = default;

private:
    bool given_ = false;
    std::optional<T> value_;
};

namespace body_detail {

using json_detail::is_optional;
using json_detail::is_vector;
using json_detail::member_path;
using json_detail::members_of;
using json_detail::missing;
using json_detail::not_a_list;
using json_detail::not_an_object;
using json_detail::wrong_type;

template <class T>
struct is_nullable : std::false_type {};
template <class T>
struct is_nullable<Nullable<T>> : std::true_type {};

template <class T>
struct is_map : std::false_type {};
template <class T>
struct is_map<std::map<std::string, T>> : std::true_type {};

template <class T>
concept Described = boost::describe::has_describe_members<T>::value;
template <class T>
concept SentWhole = requires { requires T::sent_whole; };

// Which members of a struct must be there: the ones their types say, or every one (it is sent whole).
enum class Need { typed, all };

inline std::string is_null(const std::string& path) { return std::format("field '{}' must not be null", path); }

// The member of `object` that T does not have, as the sentence that refuses it; empty if there is none.
template <class T>
std::string unknown_member(const boost::json::object& object, const std::string& path) {
    for (const auto& member : object) {
        bool known = false;
        boost::mp11::mp_for_each<members_of<T>>([&](auto described) { known = known || member.key() == described.name; });
        // The name is the caller's own text: no more of it than a name needs goes into the answer.
        if (!known) return std::format("unknown field '{}'", member_path(path, text::truncate(member.key(), 80)));
    }
    return {};
}

template <class M>
std::string read_member(const boost::json::value* v, const std::string& path, Need need, M& out);

// Reads `v`, which is there and not null, into `out`. Returns why it can't, or an empty string.
template <class T>
std::string read(const boost::json::value& v, const std::string& path, T& out) {
    if constexpr (is_vector<T>::value) {
        if (!v.is_array()) return not_a_list(path);
        std::size_t i = 0;
        for (const auto& element : v.get_array()) {
            if (auto why = read_member(&element, std::format("{}[{}]", path, i++), Need::typed, out.emplace_back()); !why.empty()) return why;
        }
        return {};
    } else if constexpr (is_map<T>::value) {
        if (!v.is_object()) return not_an_object(path);
        for (const auto& member : v.get_object()) {
            const auto key = text::truncate(member.key(), 80);
            if (auto why = read_member(&member.value(), member_path(path, key), Need::typed, out[key]); !why.empty()) return why;
        }
        return {};
    } else if constexpr (Described<T>) {
        if (!v.is_object()) return path.empty() ? std::string("The body must be a JSON object.") : not_an_object(path);
        const auto& object = v.get_object();
        // The member nobody asked for first: it is most often a misspelled one that is then also missing.
        auto why = unknown_member<T>(object, path);
        const Need need = SentWhole<T> ? Need::all : Need::typed;
        boost::mp11::mp_for_each<members_of<T>>([&](auto member) {
            if (!why.empty()) return;
            why = read_member(object.if_contains(member.name), member_path(path, member.name), need, out.*member.pointer);
        });
        return why;
    } else {
        auto converted = boost::json::try_value_to<T>(v);
        if (!converted) return wrong_type(path);
        out = std::move(*converted);
        return {};
    }
}

// The same for a member of a struct with this `need`, which may be left out (`v` is null then) or be JSON's null.
template <class M>
std::string read_member(const boost::json::value* v, const std::string& path, Need need, M& out) {
    if constexpr (is_nullable<M>::value) {
        if (!v) return need == Need::all ? missing(path) : std::string{};
        if (v->is_null()) {
            out = std::nullopt;
            return {};
        }
        typename M::value_type value{};
        auto why = read(*v, path, value);
        if (why.empty()) out = std::move(value);
        return why;
    } else if constexpr (is_optional<M>::value) {
        if (!v) return need == Need::all ? missing(path) : std::string{};
        if (v->is_null()) return need == Need::all ? std::string{} : is_null(path);
        return read(*v, path, out.emplace());
    } else {
        if (!v) return missing(path);
        if (v->is_null()) return is_null(path);
        return read(*v, path, out);
    }
}

}  // namespace body_detail

// Whether `{}` is a T: no member of it must be there. A body that may be left out altogether is such a T.
template <body_detail::Described T>
constexpr bool nothing_required() {
    bool none = !body_detail::SentWhole<T>;
    boost::mp11::mp_for_each<body_detail::members_of<T>>([&](auto member) {
        using M = std::remove_cvref_t<decltype(std::declval<T&>().*member.pointer)>;
        none = none && (body_detail::is_optional<M>::value || body_detail::is_nullable<M>::value);
    });
    return none;
}

// `text` as the body T, or the sentence for the 400.
template <class T>
std::expected<T, std::string> parse_body(std::string_view text) {
    boost::system::error_code ec;
    const auto value = boost::json::parse(text, ec);  // null if it is no JSON, which is no object either
    T body{};
    if (auto why = body_detail::read(value, {}, body); !why.empty()) return std::unexpected(std::move(why));
    return body;
}

}  // namespace poni::core
