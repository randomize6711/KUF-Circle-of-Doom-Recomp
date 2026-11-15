# kufcod_utf8_manifest(target): embeds cmake/windows/utf8.manifest (the UTF-8 process code page) into a Windows
# program (with asInvoker and the supported Windows versions). No-op elsewhere.
function(kufcod_utf8_manifest target)
    if(NOT WIN32)
        return()
    endif()
    set(_rc "${CMAKE_CURRENT_BINARY_DIR}/kufcod_utf8_manifest.rc")
    if(NOT EXISTS "${_rc}")
        file(WRITE "${_rc}" "1 24 \"${CMAKE_CURRENT_FUNCTION_LIST_DIR}/utf8.manifest\"\n")
    endif()
    target_sources(${target} PRIVATE "${_rc}")
    # The resource compiler reads the manifest: a changed manifest rebuilds the resource (not tracked otherwise).
    set_source_files_properties("${_rc}" TARGET_DIRECTORY ${target} PROPERTIES
        OBJECT_DEPENDS "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/utf8.manifest")
endfunction()
