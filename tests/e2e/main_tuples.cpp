// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#include "tuples.hpp"

#include <cstdlib>
#include <print>
#include <tuple>
#include <type_traits>

int main() {
    using namespace examples::tuples;

    // div_mod returns a real std::tuple<int32_t, int32_t>.
    auto t = div_mod(17, 5);
    static_assert(std::is_same_v<decltype(t), std::tuple<std::int32_t, std::int32_t>>,
                  "tuple type must lower to std::tuple<...>");
    if (std::get<0>(t) != 3 || std::get<1>(t) != 2) {
        std::println("div_mod(17, 5) wrong: ({}, {})", std::get<0>(t), std::get<1>(t));
        return EXIT_FAILURE;
    }

    // Destructured combine: 17/5 = 3 rem 2 → 32.
    if (combine(17, 5) != 32) {
        std::println("combine(17, 5) wrong");
        return EXIT_FAILURE;
    }

    // Wildcard destructuring picks just q.
    if (quotient(17, 5) != 3) {
        std::println("quotient(17, 5) wrong");
        return EXIT_FAILURE;
    }

    std::println("tuples OK");
    return EXIT_SUCCESS;
}
