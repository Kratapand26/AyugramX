find_package(Git REQUIRED)

set(ayu_codegen_copy_root "${CMAKE_CURRENT_BINARY_DIR}/ayu_codegen_source")
set(ayu_codegen_source "${ayu_codegen_copy_root}/codegen")

file(REMOVE_RECURSE "${ayu_codegen_copy_root}")
file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/codegen"
    DESTINATION "${ayu_codegen_copy_root}"
    PATTERN ".git" EXCLUDE)

execute_process(
    COMMAND "${GIT_EXECUTABLE}" init --quiet
    WORKING_DIRECTORY "${ayu_codegen_source}"
    RESULT_VARIABLE ayu_codegen_result
    ERROR_VARIABLE ayu_codegen_error)
if (ayu_codegen_result)
    message(FATAL_ERROR "Could not prepare code generator: ${ayu_codegen_error}")
endif()

execute_process(
    COMMAND "${GIT_EXECUTABLE}" apply
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ayu_codegen.patch"
    WORKING_DIRECTORY "${ayu_codegen_source}"
    RESULT_VARIABLE ayu_codegen_result
    ERROR_VARIABLE ayu_codegen_error)
if (ayu_codegen_result)
    message(FATAL_ERROR "Could not apply AyuGram code generator changes: ${ayu_codegen_error}")
endif()

add_subdirectory("${ayu_codegen_source}" "${CMAKE_CURRENT_BINARY_DIR}/ayu_codegen")
