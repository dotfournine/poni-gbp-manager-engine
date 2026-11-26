#include "core/etag.hpp"

#include "core/crypto.hpp"

#include <boost/json/serialize.hpp>

namespace poni::core {

std::string etag(const boost::json::object& editable) { return "\"" + sha256_hex(boost::json::serialize(editable)) + "\""; }

}  // namespace poni::core
