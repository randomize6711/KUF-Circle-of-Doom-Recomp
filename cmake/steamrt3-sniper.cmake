# The portable Linux build (docs/STEAM_DECK.md "Portable build"): this machine's Clang compiling against Valve's
# Steam Runtime 3 "sniper" SDK sysroot (Debian 11: glibc 2.31) and a libstdc++ 14.2 built in that sysroot
# (tools/toolchain/setup_sniper.sh), so the binaries run on SteamOS and on any Linux with glibc 2.31 or newer.
# Nothing is searched for on this machine: headers, libraries and pkg-config files come from the sysroot only.
#
#   tools/toolchain/setup_sniper.sh        # once: downloads and checks the sysroot, builds libstdc++ (build/toolchains)
#   cmake --preset linux-amd64-portable && cmake --build build/linux-amd64-portable
#
# The libstdc++ the binaries link against is the shared one built from the same source and configuration as the
# SDK's GCC 14 headers (the SDK itself has only a static libstdc++.a). The package carries it in bin/compat/ and
# the launcher uses it only when the system's own libstdc++ is older (tools/package/kuf-cod.sh).
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
get_filename_component(_kuf_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
include("${CMAKE_CURRENT_LIST_DIR}/steamrt3-sniper-version.cmake")
set(KUFCOD_SNIPER_SYSROOT "${_kuf_root}/build/toolchains/sniper-${KUFCOD_SNIPER_VERSION}/sysroot"
    CACHE PATH "The Steam Runtime 3 SDK sysroot")
set(KUFCOD_LIBSTDCXX "${_kuf_root}/build/toolchains/libstdcxx-${KUFCOD_LIBSTDCXX_VERSION}"
    CACHE PATH "libstdc++ built in the sysroot (include/, lib/)")
foreach(_kuf_need "${KUFCOD_SNIPER_SYSROOT}/usr/include/stdio.h" "${KUFCOD_LIBSTDCXX}/lib/libstdc++.so.6")
  if(NOT EXISTS "${_kuf_need}")
    message(FATAL_ERROR "${_kuf_need} is missing: run tools/toolchain/setup_sniper.sh first")
  endif()
endforeach()

set(CMAKE_SYSROOT "${KUFCOD_SNIPER_SYSROOT}")
set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_C_COMPILER_TARGET x86_64-linux-gnu)
set(CMAKE_CXX_COMPILER_TARGET x86_64-linux-gnu)
# The C runtime start files and libgcc of the sysroot's GCC 10 (its default compiler); the C++ library is ours.
set(_kuf_gcc "--gcc-install-dir=${KUFCOD_SNIPER_SYSROOT}/usr/lib/gcc/x86_64-linux-gnu/10")
set(_kuf_cxx_inc "${KUFCOD_LIBSTDCXX}/include/c++/14.2.0")
# Appended to the flags of the preset (-march, which then keeps CMake from using *_FLAGS_INIT): normal variables over
# the cache entries, so a reconfigure does not add them twice to the cache.
string(APPEND CMAKE_C_FLAGS " ${_kuf_gcc}")
string(APPEND CMAKE_CXX_FLAGS
       " ${_kuf_gcc} -nostdinc++ -isystem ${_kuf_cxx_inc} -isystem ${_kuf_cxx_inc}/x86_64-linux-gnu")
set(_kuf_link "-fuse-ld=lld ${_kuf_gcc} -nostdlib++ -L${KUFCOD_LIBSTDCXX}/lib")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${_kuf_link}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_kuf_link}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${_kuf_link}")
if(KUFCOD_STATIC_LIBSTDCXX)
  # The setup (installer/): one program without plugins links the C++ library in (the SDK's GCC 14.2 libstdc++.a,
  # built with -fPIC, the same version as the headers; glibc 2.31 has threads in libpthread), so it starts on any
  # glibc 2.31 system as it is.
  set(CMAKE_CXX_STANDARD_LIBRARIES
      "${KUFCOD_SNIPER_SYSROOT}/usr/lib/gcc-14/lib/gcc/x86_64-linux-gnu/14/libstdc++.a -lpthread" CACHE STRING "" FORCE)
else()
  set(CMAKE_CXX_STANDARD_LIBRARIES "-lstdc++" CACHE STRING "" FORCE)
endif()

set(CMAKE_FIND_ROOT_PATH "${KUFCOD_SNIPER_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)  # build tools (glslc, git, python) run on this machine
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
# SDL's Wayland protocol code from the sysroot's scanner (tools/toolchain/sniper-wayland-scanner: libwayland 1.18).
set(WAYLAND_SCANNER "${_kuf_root}/tools/toolchain/sniper-wayland-scanner" CACHE FILEPATH "" FORCE)
set(ENV{PKG_CONFIG_SYSROOT_DIR} "${KUFCOD_SNIPER_SYSROOT}")
set(ENV{PKG_CONFIG_LIBDIR}
    "${KUFCOD_SNIPER_SYSROOT}/usr/lib/x86_64-linux-gnu/pkgconfig:${KUFCOD_SNIPER_SYSROOT}/usr/share/pkgconfig")
set(ENV{PKG_CONFIG_PATH} "")
