# KUF: Circle of Doom native port -- ReXGlue recompiled project.
#
# SDK boilerplate lives in generated/rexglue.cmake (managed by `rexglue migrate`).
# Recompiled sources in generated/default/ are produced from the user's own default.xex and are never committed.

cmake_minimum_required(VERSION 3.25)
project(kuf_cod LANGUAGES C CXX)

# The port's version (src/runtime/version.h): the base here, the commit added at build time.
set(KUFCOD_BASE_VERSION "0.7.0")

# Published binaries carry no build-machine paths: __FILE__ (asserts, log locations) relative to the source tree,
# for this project and the SDK alike (debug information is unchanged, and stripped from packages).
add_compile_options("-fmacro-prefix-map=${CMAKE_SOURCE_DIR}/=")

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Our SDK patches (patches/rexglue-sdk) must be applied to the submodule: tools/apply_sdk_patches.sh.
execute_process(COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tools/apply_sdk_patches.sh --check
                RESULT_VARIABLE _kufcod_patch_state OUTPUT_VARIABLE _kufcod_patch_out)
if(NOT _kufcod_patch_state EQUAL 0)
    message(FATAL_ERROR "ReXGlue SDK patches missing; run tools/apply_sdk_patches.sh\n${_kufcod_patch_out}")
endif()

include(generated/rexglue.cmake)

# generated/rexglue.cmake only adds the recompiled sources if they exist at configure time. In a fresh
# tree, the first build runs codegen, and CMake must be re-run before the game can link.
if(NOT REXGLUE_ENTRYPOINT_GENERATED_SOURCES)
    message(WARNING
        "No recompiled sources in generated/default yet. Build the 'kuf_cod_codegen' target "
        "(or run tools/codegen_iterate.sh), then re-run CMake before building kuf_cod.")
endif()

set(KUFCOD_SOURCES
    src/main.cpp
    src/runtime/crash_report.cpp
    src/runtime/eh_trace.cpp
    src/runtime/telemetry.cpp
    src/runtime/version.cpp
    src/runtime/frame_capture.cpp
    src/runtime/png_writer.cpp
    src/runtime/fatal_trap.cpp
    src/runtime/audio_meter.cpp
    src/runtime/save_groups.cpp
    src/runtime/loot_test.cpp
    src/runtime/texture_free_trace.cpp
    src/runtime/profiler.cpp
    src/runtime/frame_pacing.cpp
    src/runtime/render_frames.cpp
    src/runtime/presentation.cpp
    src/runtime/presentation_math.cpp
    src/runtime/menu_trace.cpp
    src/runtime/menu_pointer.cpp
    src/runtime/menu_pointer_math.cpp
    src/runtime/title_menu.cpp
    src/coop/coop_runtime.cpp
    src/runtime/lobby_screen.cpp
    src/runtime/prompt_text.cpp
    src/runtime/controls_screen.cpp
    src/runtime/menu_mouse.cpp
    src/runtime/ui_layout.cpp
    src/runtime/update_status.cpp
    src/runtime/pc_settings_menu.cpp
    src/runtime/camera_trace.cpp
    src/runtime/camera_math.cpp
    src/runtime/modern_camera.cpp
    src/runtime/pc_settings.cpp
    src/runtime/pc_settings_runtime.cpp
    src/runtime/display.cpp
    src/runtime/display_window.cpp
    src/runtime/gpu_defaults.cpp
    src/runtime/display_math.cpp
    src/input/keyboard_pad.cpp
    src/input/key_routing.cpp
    src/input/pad_remap.cpp
    src/input/pad_remap_driver.cpp
    src/input/button_prompts.cpp
    src/input/kuf_input_driver.cpp
    src/input/input_factory.cpp
    src/input/input_script.cpp
    src/input/input_script_parse.cpp
    src/input/game_input_hooks.cpp
    src/input/game_context.cpp
)

# Diagnostic import audit (Linux only): wraps every game import to count calls (src/runtime/import_audit.cpp).
option(KUFCOD_IMPORT_AUDIT "Build with the diagnostic import-call audit (Linux only)" OFF)
if(KUFCOD_IMPORT_AUDIT)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        message(FATAL_ERROR "KUFCOD_IMPORT_AUDIT is only supported on Linux")
    endif()
    list(APPEND KUFCOD_SOURCES src/runtime/import_audit.cpp)
else()
    list(APPEND KUFCOD_SOURCES src/runtime/import_audit_off.cpp)
endif()

if(WIN32)
    add_executable(kuf_cod WIN32 ${KUFCOD_SOURCES})
else()
    add_executable(kuf_cod ${KUFCOD_SOURCES})
endif()

# The lobby layer (src/net, docs/MULTIPLAYER.md). The Steam backend is compiled against a Steamworks SDK the player
# downloaded (never committed, never redistributed): KUF_STEAMWORKS_SDK, by default build/tools/steamworks_sdk_*/sdk
# when present; without it the backend is built as "not available". Valve's library is never linked: it is loaded
# at run time only when an App ID is configured.
add_library(kuf_net STATIC src/net/crypto.cpp src/net/protocol.cpp src/net/session.cpp src/net/steam_lobby.cpp
            src/net/steam_backend.cpp src/coop/coop_addr.cpp src/coop/coop_sock.cpp
            src/coop/coop_xsession.cpp src/coop/lobby_service.cpp src/coop/steam_provider.cpp
            src/coop/lobby_menu.cpp src/coop/text_field.cpp src/input/text_entry.cpp)
target_include_directories(kuf_net PUBLIC src)
if(NOT DEFINED KUF_STEAMWORKS_SDK)
    file(GLOB _kuf_sdk_dirs LIST_DIRECTORIES true "${CMAKE_CURRENT_SOURCE_DIR}/build/tools/steamworks_sdk_*/sdk")
    if(_kuf_sdk_dirs)
        list(GET _kuf_sdk_dirs -1 KUF_STEAMWORKS_SDK)
    endif()
endif()
if(KUF_STEAMWORKS_SDK AND EXISTS "${KUF_STEAMWORKS_SDK}/public/steam/steam_api_flat.h")
    message(STATUS "Steamworks SDK: ${KUF_STEAMWORKS_SDK} (the Steam lobby backend is built)")
    target_include_directories(kuf_net SYSTEM PRIVATE "${KUF_STEAMWORKS_SDK}/public")
    target_compile_definitions(kuf_net PRIVATE KUF_HAVE_STEAMWORKS=1)
    target_link_libraries(kuf_net PRIVATE ${CMAKE_DL_LIBS})
else()
    message(STATUS "Steamworks SDK: not found (the Steam lobby backend is not built)")
endif()
target_link_libraries(kuf_cod PRIVATE kuf_net)

# The version header, regenerated at every build (written only when the version changes).
set(KUFCOD_VERSION_HEADER "${CMAKE_BINARY_DIR}/generated_include/kuf_version_generated.h")
add_custom_target(kuf_version
    COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR} -DOUTPUT=${KUFCOD_VERSION_HEADER}
            -DBASE_VERSION=${KUFCOD_BASE_VERSION} -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/kuf_version.cmake
    BYPRODUCTS ${KUFCOD_VERSION_HEADER}
    COMMENT "Version header")
add_dependencies(kuf_cod kuf_version)
target_include_directories(kuf_cod PRIVATE ${CMAKE_BINARY_DIR}/generated_include)

# rex/rex_app.h includes the SDK's ImGui headers, but rex::runtime does not propagate ImGui's include path
# (rexui is an object library folded into the runtime). Take only the usage requirement, not the objects.
target_include_directories(kuf_cod PRIVATE $<TARGET_PROPERTY:imgui,INTERFACE_INCLUDE_DIRECTORIES>)
# toml++ (header-only, part of the SDK's third-party code): pc_settings.toml (src/runtime/pc_settings.cpp).
target_include_directories(kuf_cod PRIVATE ${REXSDK_DIR}/thirdparty/tomlplusplus/include)
if(KUFCOD_IMPORT_AUDIT)
    target_link_libraries(kuf_cod PRIVATE ${CMAKE_DL_LIBS})
    target_compile_definitions(kuf_cod PRIVATE KUFCOD_IMPORT_AUDIT=1)  # fatal_trap.cpp: wrappers own __imp__
endif()

rexglue_setup_target(kuf_cod GPU_PLUGINS xenos)

# The SDK writes its binaries into its own source tree (thirdparty/rexglue-sdk/out/<platform>), shared by
# every build configuration. Keep all binaries of this build tree together in <build>/bin instead, so the
# executable finds librexruntime via its $ORIGIN rpath and presets do not overwrite each other.
set(KUFCOD_BIN_DIR "${CMAKE_BINARY_DIR}/bin")
foreach(_kufcod_target kuf_cod rexruntime rexglue rexgpu-xenos TracyClient)
    if(TARGET ${_kufcod_target})
        set_target_properties(${_kufcod_target} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${KUFCOD_BIN_DIR}"
            LIBRARY_OUTPUT_DIRECTORY "${KUFCOD_BIN_DIR}")
    endif()
endforeach()

option(KUFCOD_BUILD_TESTS "Build focused tests for the recompiled image" ON)
if(KUFCOD_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
