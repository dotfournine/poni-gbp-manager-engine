#pragma once

#include <string_view>

namespace poni::core {

// Set by CMake from the project version.
inline constexpr std::string_view kVersion = PONI_VERSION;

}  // namespace poni::core
