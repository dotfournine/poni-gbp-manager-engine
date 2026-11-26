// The strong validator of one editable thing (D-108): the ETag its single GET
// answers with and its change compares If-Match with. It hashes the members a
// person can change and nothing the engine writes by itself, so that a sync or
// a count does not make a person's view stale.
#pragma once

#include <boost/json/object.hpp>

#include <string>

namespace poni::core {

// "\"" + sha256_hex(serialize(editable)) + "\"". Boost.JSON keeps insertion order and the
// tag follows it, so the caller builds `editable` in one fixed member order.
std::string etag(const boost::json::object& editable);

}  // namespace poni::core
