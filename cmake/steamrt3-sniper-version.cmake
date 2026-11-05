# The portable build's pinned inputs (cmake/steamrt3-sniper.cmake, tools/toolchain/setup_sniper.sh reads these too).
# Steam Runtime 3 "sniper" SDK sysroot: https://repo.steampowered.com/steamrt3/images/<version>/
set(KUFCOD_SNIPER_VERSION 3.0.20260928.262393)
set(KUFCOD_SNIPER_SDK_SHA256 adb13b9e124e73e0b2cf25e0022703978ec8e917b9a192dcec61bebd72479521)
# GNU GCC source for the shared libstdc++ (the SDK's own GCC 14 is 14.2.0 and ships only a static libstdc++.a).
set(KUFCOD_LIBSTDCXX_VERSION 14.2.0)
set(KUFCOD_GCC_SOURCE_SHA256 a7b39bc69cbf9e25826c5a60ab26477001f7c08d85cec04bc0e29cabed6f3cc9)
# The oldest C library the portable binaries may need (the sysroot's glibc); checked after every portable build.
set(KUFCOD_GLIBC_BASELINE 2.31)
