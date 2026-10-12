find_package(Git REQUIRED)

set(ayu_lib_ui_copy_root "${CMAKE_CURRENT_BINARY_DIR}/ayu_lib_ui_source")
set(ayu_lib_ui_source "${ayu_lib_ui_copy_root}/lib_ui")

file(REMOVE_RECURSE "${ayu_lib_ui_copy_root}")
file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/lib_ui"
    DESTINATION "${ayu_lib_ui_copy_root}"
    PATTERN ".git" EXCLUDE)

file(GLOB_RECURSE ayu_lib_ui_inputs CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/lib_ui/*")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${ayu_lib_ui_inputs})

execute_process(
    COMMAND "${GIT_EXECUTABLE}" init --quiet
    WORKING_DIRECTORY "${ayu_lib_ui_source}"
    RESULT_VARIABLE ayu_lib_ui_result
    ERROR_VARIABLE ayu_lib_ui_error)
if (ayu_lib_ui_result)
    message(FATAL_ERROR "Could not prepare UI library: ${ayu_lib_ui_error}")
endif()

set(ayu_lib_ui_patches ayu_lib_ui_features.patch)
if (WIN32)
    list(APPEND ayu_lib_ui_patches ayu_lib_ui.patch)
endif()

foreach (patch IN LISTS ayu_lib_ui_patches)
    set(ayu_lib_ui_patch "${ayu_lib_ui_copy_root}/${patch}")
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/${patch}"
        "${ayu_lib_ui_patch}"
        @ONLY NEWLINE_STYLE UNIX)

    execute_process(
        COMMAND "${GIT_EXECUTABLE}" apply "${ayu_lib_ui_patch}"
        WORKING_DIRECTORY "${ayu_lib_ui_source}"
        RESULT_VARIABLE ayu_lib_ui_result
        ERROR_VARIABLE ayu_lib_ui_error)
    if (ayu_lib_ui_result)
        message(FATAL_ERROR "Could not apply ${patch}: ${ayu_lib_ui_error}")
    endif()
endforeach()

add_subdirectory("${ayu_lib_ui_source}" "${CMAKE_CURRENT_BINARY_DIR}/lib_ui")
