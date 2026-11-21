# Linux, Clang (from apt.llvm.org in the container image) with libstdc++.
set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)
# Include vcpkg's own Linux toolchain (a chainload toolchain replaces it; see
# brew-llvm.cmake for why that matters), after the compilers are chosen.
foreach(_poni_root IN ITEMS "${_VCPKG_ROOT_DIR}" "${Z_VCPKG_ROOT_DIR}" "$ENV{VCPKG_ROOT}")
  if(_poni_root AND EXISTS "${_poni_root}/scripts/toolchains/linux.cmake")
    include("${_poni_root}/scripts/toolchains/linux.cmake")
    break()
  endif()
endforeach()
