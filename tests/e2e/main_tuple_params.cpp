// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#include "tuple_params.hpp"

#include <cstdlib>
#include <print>
#include <tuple>

int main() {
    using namespace examples::tuple_params;

    // Two-arg pair destructured at the param slot: 7 + 5 = 12.
    if (add_pair(std::tuple<std::int32_t, std::int32_t>{7, 5}) != 12) {
        std::println("add_pair wrong");
        return EXIT_FAILURE;
    }

    // Nested tuple param: ((1, 2), 10) → 13.
    using outer_t = std::tuple<std::tuple<std::int32_t, std::int32_t>, std::int32_t>;
    if (sum_pair_and_third(outer_t{{1, 2}, 10}) != 13) {
        std::println("sum_pair_and_third wrong");
        return EXIT_FAILURE;
    }

    // Match-arm payload destructuring.
    Action q{Action::quit_t{}};
    if (score(q) != 0) {
        std::println("quit wrong: {}", score(q));
        return EXIT_FAILURE;
    }

    Action p{Action::point_t{std::tuple<std::int32_t, std::int32_t>{3, 4}}};
    if (score(p) != 7) {
        std::println("point wrong: {}", score(p));
        return EXIT_FAILURE;
    }

    Action r{Action::region_t{outer_t{{1, 2}, 10}}};
    if (score(r) != 13) {
        std::println("region wrong: {}", score(r));
        return EXIT_FAILURE;
    }

    std::println("tuple params OK");
    return EXIT_SUCCESS;
}
