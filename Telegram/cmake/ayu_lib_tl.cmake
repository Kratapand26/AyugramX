find_package(Git REQUIRED)

set(ayu_lib_tl_generator_root "${CMAKE_CURRENT_BINARY_DIR}/ayu_lib_tl_generator")
set(ayu_lib_tl_generator "${ayu_lib_tl_generator_root}/tl/generate_tl.py")

file(REMOVE_RECURSE "${ayu_lib_tl_generator_root}")
file(MAKE_DIRECTORY "${ayu_lib_tl_generator_root}/tl")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/lib_tl/tl/generate_tl.py"
    "${ayu_lib_tl_generator}"
    COPYONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ayu_lib_tl.patch")

execute_process(
    COMMAND "${GIT_EXECUTABLE}" init --quiet
    WORKING_DIRECTORY "${ayu_lib_tl_generator_root}"
    RESULT_VARIABLE ayu_lib_tl_result
    ERROR_VARIABLE ayu_lib_tl_error)
if (ayu_lib_tl_result)
    message(FATAL_ERROR "Could not prepare TL generator: ${ayu_lib_tl_error}")
endif()

execute_process(
    COMMAND "${GIT_EXECUTABLE}" apply
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ayu_lib_tl.patch"
    WORKING_DIRECTORY "${ayu_lib_tl_generator_root}"
    RESULT_VARIABLE ayu_lib_tl_result
    ERROR_VARIABLE ayu_lib_tl_error)
if (ayu_lib_tl_result)
    message(FATAL_ERROR "Could not apply AyuGram TL generator changes: ${ayu_lib_tl_error}")
endif()
