// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#include "opaque.hpp"

#include <cstdlib>
#include <print>
#include <type_traits>

int main() {
    using namespace examples::newtype;

    auto u = make_user(42);
    auto p = make_post(42);

    // Round-trip through .value.
    if (raw_user(u) != 42) {
        std::println("raw_user round-trip wrong");
        return EXIT_FAILURE;
    }
    if (raw_post(p) != 42) {
        std::println("raw_post round-trip wrong");
        return EXIT_FAILURE;
    }

    // Each opaque is its own type at the C++ layer — UserId and
    // PostId are NOT convertible across, so this static_assert
    // pins the distinctness.
    static_assert(!std::is_same_v<UserId, PostId>,
                  "opaque types over the same underlying must be distinct C++ types");
    static_assert(!std::is_convertible_v<UserId, PostId>,
                  "opaque types must not implicit-convert across the boundary");

    std::println("opaque OK");
    return EXIT_SUCCESS;
}
