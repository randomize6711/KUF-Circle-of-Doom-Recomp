# The graphical setup (installer/README.md): a GTK 3 program, built on its own (it does not need the recompiled game):
#   cmake -S installer -B build/installer -G Ninja && ninja -C build/installer
# Targets: kuf_setup_core (the tested logic), kuf-setup (the program), kuf_setup_tests (unit tests, no GTK window).
cmake_minimum_required(VERSION 3.25)
project(kuf_setup CXX)

# Published binaries carry no build-machine paths (__FILE__ relative to the source tree).
add_compile_options("-fmacro-prefix-map=${CMAKE_SOURCE_DIR}/=")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GLIB REQUIRED IMPORTED_TARGET glib-2.0)
pkg_check_modules(GTK3 REQUIRED IMPORTED_TARGET gtk+-3.0)
pkg_check_modules(FONTCONFIG REQUIRED IMPORTED_TARGET fontconfig)

add_library(kuf_setup_core STATIC
    src/core/util.cpp
    src/core/game_data.cpp
    src/core/vdf.cpp
    src/core/steam.cpp
    src/core/install.cpp)
target_include_directories(kuf_setup_core PUBLIC src)
target_link_libraries(kuf_setup_core PUBLIC PkgConfig::GLIB)
target_compile_options(kuf_setup_core PRIVATE -Wall -Wextra)

add_executable(kuf-setup src/gui/main.cpp src/gui/unattended.cpp)
target_link_libraries(kuf-setup PRIVATE kuf_setup_core PkgConfig::GTK3 PkgConfig::FONTCONFIG)
target_compile_options(kuf-setup PRIVATE -Wall -Wextra -Wno-deprecated-declarations)

# Unit tests (Catch2 from the pinned SDK, as the game's tests).
set(KUF_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/rexglue-sdk")
if(EXISTS "${KUF_SDK_DIR}/thirdparty/catch2/CMakeLists.txt")
    add_subdirectory(${KUF_SDK_DIR}/thirdparty/catch2 catch2 EXCLUDE_FROM_ALL)
    add_executable(kuf_setup_tests tests/test_core.cpp tests/test_steam.cpp tests/test_install.cpp)
    target_link_libraries(kuf_setup_tests PRIVATE kuf_setup_core Catch2::Catch2WithMain)
    target_compile_definitions(kuf_setup_tests PRIVATE
        KUF_SETUP_ASSETS="${CMAKE_CURRENT_SOURCE_DIR}/assets")
    add_executable(kuf_setup_e2e tests/e2e_install.cpp)  # a full installation (installer/README.md "Testing")
    target_link_libraries(kuf_setup_e2e PRIVATE kuf_setup_core)
    enable_testing()
    add_test(NAME setup.unit COMMAND kuf_setup_tests)
endif()
