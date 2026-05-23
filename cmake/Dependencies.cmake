# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 James Kane

# Third-party dependencies, fetched via FetchContent so contributors don't have
# to install anything beyond a C++23 toolchain, CMake, and Ninja.

include(FetchContent)

# doctest: tiny, fast, header-only unit-test framework. The version pin (and
# CMAKE_POLICY_VERSION_MINIMUM) is required because doctest's own
# CMakeLists.txt declares cmake_minimum_required(VERSION 3.0) — and CMake 4+
# rejects compatibility with versions older than 3.5 unless we opt in.
FetchContent_Declare(
    doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG        v2.4.11
    GIT_SHALLOW    TRUE)

# Populate only when tests are built — avoids the clone on a tests-off build.
function(vestra_fetch_doctest)
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)
    set(DOCTEST_NO_INSTALL ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(doctest)
endfunction()
