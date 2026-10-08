if (NOT WIN32)
    return()
endif()

find_package(Git REQUIRED)

set(ayu_lib_ui_source_root "${CMAKE_CURRENT_BINARY_DIR}/ayu_lib_ui_source")
set(ayu_lib_ui_original "${CMAKE_CURRENT_SOURCE_DIR}/lib_ui/ui/widgets/popup_menu.cpp")
set(ayu_lib_ui_patched "${ayu_lib_ui_source_root}/ui/widgets/popup_menu.cpp")

configure_file("${ayu_lib_ui_original}" "${ayu_lib_ui_patched}" COPYONLY)
set(ayu_lib_ui_patch "${ayu_lib_ui_source_root}/ayu_lib_ui.patch")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ayu_lib_ui.patch"
    "${ayu_lib_ui_patch}"
    @ONLY NEWLINE_STYLE UNIX)

execute_process(
    COMMAND "${GIT_EXECUTABLE}" init --quiet
    WORKING_DIRECTORY "${ayu_lib_ui_source_root}"
    RESULT_VARIABLE ayu_lib_ui_result
    ERROR_VARIABLE ayu_lib_ui_error)
if (ayu_lib_ui_result)
    message(FATAL_ERROR "Could not prepare popup menu source: ${ayu_lib_ui_error}")
endif()

execute_process(
    COMMAND "${GIT_EXECUTABLE}" apply "${ayu_lib_ui_patch}"
    WORKING_DIRECTORY "${ayu_lib_ui_source_root}"
    RESULT_VARIABLE ayu_lib_ui_result
    ERROR_VARIABLE ayu_lib_ui_error)
if (ayu_lib_ui_result)
    message(FATAL_ERROR "Could not apply Windows submenu changes: ${ayu_lib_ui_error}")
endif()

get_target_property(ayu_lib_ui_sources lib_ui SOURCES)
list(FIND ayu_lib_ui_sources "${ayu_lib_ui_original}" ayu_lib_ui_source_index)
if (ayu_lib_ui_source_index LESS 0)
    message(FATAL_ERROR "Could not find lib_ui popup menu source")
endif()
list(REMOVE_ITEM ayu_lib_ui_sources "${ayu_lib_ui_original}")
set_property(TARGET lib_ui PROPERTY SOURCES "${ayu_lib_ui_sources}")
target_sources(lib_ui PRIVATE "${ayu_lib_ui_patched}")
