// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#include "map_filter.hpp"

#include <cstdlib>
#include <print>

int main() {
    using namespace examples::map_filter;

    // Stepper(1, +1, <6) yields 1,2,3,4,5. Doubled: 2,4,6,8,10. Sum=30.
    if (auto v = sum_doubled(); v != 30) {
        std::println("sum_doubled expected 30, got {}", v);
        return EXIT_FAILURE;
    }
    // Stepper 1..10, evens: 2,4,6,8,10. Sum=30.
    if (auto v = sum_evens(); v != 30) {
        std::println("sum_evens expected 30, got {}", v);
        return EXIT_FAILURE;
    }
    // Even squares from 1..10: 4 + 16 + 36 + 64 + 100 = 220.
    if (auto v = sum_even_squares(); v != 220) {
        std::println("sum_even_squares expected 220, got {}", v);
        return EXIT_FAILURE;
    }
    // Stepper 1..5 = 1,2,3,4. Scaled by factor=3: 3,6,9,12. Sum=30.
    if (auto v = sum_scaled(); v != 30) {
        std::println("sum_scaled expected 30, got {}", v);
        return EXIT_FAILURE;
    }

    std::println("map_filter OK");
    return EXIT_SUCCESS;
}
