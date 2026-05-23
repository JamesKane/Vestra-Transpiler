# Project-wide warning configuration.
#
# Usage:
#   include(CompilerWarnings)
#   vestra_apply_warnings(<target>)
#
# We compile with -Werror by default; flip VESTRA_WARNINGS_AS_ERRORS=OFF when
# bisecting or porting to a new compiler version.

option(VESTRA_WARNINGS_AS_ERRORS "Treat warnings as errors" ON)

function(vestra_apply_warnings target)
    if(NOT TARGET ${target})
        message(FATAL_ERROR "vestra_apply_warnings: '${target}' is not a target")
    endif()

    set(clang_gcc_warnings
        -Wall
        -Wextra
        -Wpedantic
        -Wshadow
        -Wnon-virtual-dtor
        -Wold-style-cast
        -Wcast-align
        -Wunused
        -Woverloaded-virtual
        -Wconversion
        -Wsign-conversion
        -Wnull-dereference
        -Wdouble-promotion
        -Wformat=2
        -Wimplicit-fallthrough
        -Wmissing-declarations
    )

    set(clang_warnings ${clang_gcc_warnings})

    set(gcc_warnings
        ${clang_gcc_warnings}
        -Wmisleading-indentation
        -Wduplicated-cond
        -Wduplicated-branches
        -Wlogical-op
        -Wuseless-cast
    )

    if(VESTRA_WARNINGS_AS_ERRORS)
        list(APPEND clang_warnings -Werror)
        list(APPEND gcc_warnings   -Werror)
    endif()

    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(${target} PRIVATE ${clang_warnings})
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(${target} PRIVATE ${gcc_warnings})
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(${target} PRIVATE /W4 /permissive-)
        if(VESTRA_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    endif()
endfunction()
