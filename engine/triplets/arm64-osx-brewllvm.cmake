# arm64 macOS, static libraries, built with Homebrew LLVM (see cmake/brew-llvm.cmake).
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Darwin)
set(VCPKG_OSX_ARCHITECTURES arm64)
set(VCPKG_CHAINLOAD_TOOLCHAIN_FILE "${CMAKE_CURRENT_LIST_DIR}/../cmake/brew-llvm.cmake")

# cctz includes CoreFoundation to find the machine's own time zone. Upstream
# clang rejects an enum declaration in those SDK headers when compiling C++
# (Apple's clang accepts it); the declaration is harmless.
if(PORT STREQUAL "cctz")
  set(VCPKG_CXX_FLAGS "-Wno-elaborated-enum-base")
  set(VCPKG_C_FLAGS "")
endif()
