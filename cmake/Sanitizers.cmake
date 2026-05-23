# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 James Kane

# Sanitizer integration. Enable per build with -DVESTRA_SANITIZE=address+ub.
#
# The setting is a '+'-separated list of: address, undefined, thread, leak.
# 'address' + 'thread' are mutually exclusive; we error if both are requested.
#
# Apply to a target with vestra_apply_sanitizers(<target>).

set(VESTRA_SANITIZE "" CACHE STRING
    "Sanitizers to enable; '+'-separated subset of: address, undefined, thread, leak")

function(vestra_apply_sanitizers target)
    if(NOT VESTRA_SANITIZE)
        return()
    endif()

    string(REPLACE "+" ";" requested "${VESTRA_SANITIZE}")
    set(supported address undefined thread leak)
    set(flags "")

    if("address" IN_LIST requested AND "thread" IN_LIST requested)
        message(FATAL_ERROR "AddressSanitizer and ThreadSanitizer cannot be combined")
    endif()

    foreach(san IN LISTS requested)
        if(NOT san IN_LIST supported)
            message(FATAL_ERROR "Unknown sanitizer '${san}'. Supported: ${supported}")
        endif()
        list(APPEND flags "-fsanitize=${san}")
    endforeach()

    if(flags)
        list(APPEND flags -fno-omit-frame-pointer -g)
        target_compile_options(${target} PRIVATE ${flags})
        target_link_options(${target}    PRIVATE ${flags})
        message(STATUS "Sanitizers for ${target}: ${VESTRA_SANITIZE}")
    endif()
endfunction()
