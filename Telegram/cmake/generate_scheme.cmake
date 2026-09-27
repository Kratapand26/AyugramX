# This file is part of Telegram Desktop,
# the official desktop application for the Telegram messaging service.
#
# For license and copyright information please follow this link:
# https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL

function(generate_scheme target_name script scheme_files)
    find_package(Python3 REQUIRED)

    set(gen_dst ${CMAKE_CURRENT_BINARY_DIR}/gen)
    file(MAKE_DIRECTORY ${gen_dst})

    set(gen_timestamp ${gen_dst}/scheme.timestamp)
    set(gen_files
        ${gen_dst}/scheme.cpp
        ${gen_dst}/scheme.h
        ${gen_dst}/scheme-dump_to_text.cpp
        ${gen_dst}/scheme-dump_to_text.h
    )

    add_custom_command(
    OUTPUT
        ${gen_timestamp}
    BYPRODUCTS
        ${gen_files}
    COMMAND
        ${CMAKE_COMMAND} -E env
        "AYU_LIB_TL_PATH=${ayu_lib_tl_generator_root}/tl"
        ${Python3_EXECUTABLE}
        ${script}
        -o${gen_dst}/scheme
        ${scheme_files}
    COMMENT "Generating scheme (${target_name})"
    DEPENDS
        ${script}
        ${ayu_lib_tl_generator_root}/tl/generate_tl.py
        ${scheme_files}
    VERBATIM
    )
    generate_target(${target_name} scheme ${gen_timestamp} "${gen_files}" ${gen_dst})
endfunction()
