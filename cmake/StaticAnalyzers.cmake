# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 James Kane

# Optional static-analysis hooks. clang-tidy / clang-format integration is
# additive: it activates if the tool is found on PATH, and is silent otherwise
# so the build never breaks because a developer hasn't installed LLVM utilities.

option(VESTRA_ENABLE_CLANG_TIDY "Run clang-tidy during the build" OFF)

find_program(VESTRA_CLANG_FORMAT NAMES clang-format)
find_program(VESTRA_CLANG_TIDY   NAMES clang-tidy)

if(VESTRA_ENABLE_CLANG_TIDY)
    if(VESTRA_CLANG_TIDY)
        set(CMAKE_CXX_CLANG_TIDY
            "${VESTRA_CLANG_TIDY};--use-color"
            CACHE STRING "clang-tidy command line" FORCE)
        message(STATUS "clang-tidy: ${VESTRA_CLANG_TIDY} (build-time)")
    else()
        message(WARNING "VESTRA_ENABLE_CLANG_TIDY=ON but clang-tidy was not found")
    endif()
endif()

# `cmake --build build --target format`  formats every source file in tree.
function(vestra_add_format_target)
    if(NOT VESTRA_CLANG_FORMAT)
        return()
    endif()

    file(GLOB_RECURSE format_sources
         CONFIGURE_DEPENDS
         "${CMAKE_SOURCE_DIR}/include/*.hpp"
         "${CMAKE_SOURCE_DIR}/include/*.h"
         "${CMAKE_SOURCE_DIR}/src/*.cpp"
         "${CMAKE_SOURCE_DIR}/src/*.hpp"
         "${CMAKE_SOURCE_DIR}/src/*.h"
         "${CMAKE_SOURCE_DIR}/tests/*.cpp"
         "${CMAKE_SOURCE_DIR}/tests/*.hpp")

    add_custom_target(format
        COMMAND ${VESTRA_CLANG_FORMAT} -i --style=file ${format_sources}
        COMMENT "clang-format: rewriting ${CMAKE_SOURCE_DIR}/{include,src,tests}"
        VERBATIM)

    add_custom_target(format-check
        COMMAND ${VESTRA_CLANG_FORMAT} --dry-run -Werror --style=file ${format_sources}
        COMMENT "clang-format: checking ${CMAKE_SOURCE_DIR}/{include,src,tests}"
        VERBATIM)
endfunction()

# `cmake --build build --target tidy` runs clang-tidy against compile_commands.json.
function(vestra_add_tidy_target)
    if(NOT VESTRA_CLANG_TIDY)
        return()
    endif()

    file(GLOB_RECURSE tidy_sources
         CONFIGURE_DEPENDS
         "${CMAKE_SOURCE_DIR}/src/*.cpp"
         "${CMAKE_SOURCE_DIR}/tests/*.cpp")

    add_custom_target(tidy
        COMMAND ${VESTRA_CLANG_TIDY}
                -p ${CMAKE_BINARY_DIR}
                --quiet
                ${tidy_sources}
        COMMENT "clang-tidy: linting ${CMAKE_SOURCE_DIR}/{src,tests}"
        VERBATIM)
endfunction()
