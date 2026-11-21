# Toolchain: Homebrew LLVM clang on macOS (arm64).
#
# Used twice, so the app and every vcpkg port are built by the same compiler
# and the same libc++ headers:
#   - by the project, via VCPKG_CHAINLOAD_TOOLCHAIN_FILE in CMakePresets.json
#   - by vcpkg ports, via the overlay triplet triplets/arm64-osx-brewllvm.cmake
#
# libc++ choice: Homebrew clang compiles against its own (newer) libc++ headers
# but links the macOS system libc++ dylib by default. Header features that
# need newer dylib symbols are gated by Apple "availability" markup. That is
# Homebrew's recommended setup and what we use here; see FINDINGS.md.

if(DEFINED ENV{PONI_LLVM_PREFIX})
  set(_poni_llvm "$ENV{PONI_LLVM_PREFIX}")
else()
  set(_poni_llvm "/opt/homebrew/opt/llvm")
endif()

set(CMAKE_C_COMPILER   "${_poni_llvm}/bin/clang")
set(CMAKE_CXX_COMPILER "${_poni_llvm}/bin/clang++")
set(CMAKE_AR           "${_poni_llvm}/bin/llvm-ar")
set(CMAKE_RANLIB       "${_poni_llvm}/bin/llvm-ranlib")

# A chainload toolchain replaces vcpkg's own osx.cmake, which is what sets
# CMAKE_SYSTEM_PROCESSOR, the VCPKG_*_FLAGS and -fPIC for port builds. Without
# it CMAKE_SYSTEM_PROCESSOR is empty and Boost.Context falls back to x86_64
# assembly. So include it. Port builds pass _VCPKG_ROOT_DIR; try_compile and
# the project build fall back to $VCPKG_ROOT.
foreach(_poni_root IN ITEMS "${_VCPKG_ROOT_DIR}" "${Z_VCPKG_ROOT_DIR}" "$ENV{VCPKG_ROOT}")
  if(_poni_root AND EXISTS "${_poni_root}/scripts/toolchains/osx.cmake")
    include("${_poni_root}/scripts/toolchains/osx.cmake")
    break()
  endif()
endforeach()
if(NOT CMAKE_SYSTEM_PROCESSOR)
  set(CMAKE_SYSTEM_PROCESSOR arm64 CACHE STRING "" FORCE)
endif()
if(NOT CMAKE_OSX_ARCHITECTURES)
  set(CMAKE_OSX_ARCHITECTURES arm64 CACHE STRING "")
endif()
