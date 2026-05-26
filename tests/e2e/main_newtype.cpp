// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#include "newtype.hpp"

#include <cstdlib>
#include <print>
#include <unordered_map>

int main() {
    using namespace examples::newtype;

    auto a = UserId{42};
    auto b = UserId{42};
    auto c = UserId{99};

    // Eq comes from `enum class Q : T {}` automatically.
    if (!sameUser(a, b) || sameUser(a, c)) {
        std::println("eq compare wrong");
        return EXIT_FAILURE;
    }

    // Display via derive(Debug) → `UserId(42)`.
    auto s = label(a);
    if (s != "user=UserId(42)") {
        std::println("label wrong: '{}'", s);
        return EXIT_FAILURE;
    }

    // Round-trip through the inverse conversion.
    if (roundTrip(7) != 7 || roundTrip(0) != 0 || roundTrip(0xFFFFFFFFu) != 0xFFFFFFFFu) {
        std::println("roundTrip wrong");
        return EXIT_FAILURE;
    }

    // Use in an unordered_map keyed by an opaque type — proves the
    // derive(Hash) spec is reachable.
    std::unordered_map<UserId, int> ages;
    ages[UserId{1}] = 30;
    ages[UserId{2}] = 25;
    if (ages.size() != 2 || ages[UserId{1}] != 30 || ages[UserId{2}] != 25) {
        std::println("unordered_map wrong");
        return EXIT_FAILURE;
    }

    std::println("opaque newtype OK");
    return EXIT_SUCCESS;
}
