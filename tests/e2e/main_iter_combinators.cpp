// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#include "iter_combinators.hpp"

#include <cstdlib>
#include <print>

int main() {
    using namespace examples::iter_combinators;

    // zip stops at the shorter iterator. Countdown(5) yields 5,4,3,2,1
    // (5 values). Stepper(10, +1, <100) yields 10,11,12,… (90 values).
    // The pairs are (5,10), (4,11), (3,12), (2,13), (1,14); sum of
    // i*j = 50 + 44 + 36 + 26 + 14 = 170.
    if (auto v = zip_sum(); v != 170) {
        std::println("zip_sum expected 170, got {}", v);
        return EXIT_FAILURE;
    }
    // take(Stepper(0, +7, <1000), 3) yields 0, 7, 14; sum = 21.
    if (auto v = take_sum(); v != 21) {
        std::println("take_sum expected 21, got {}", v);
        return EXIT_FAILURE;
    }
    // take(zip(Stepper(1, +1, <1000), Countdown(100)), 4) yields
    // (1,100), (2,99), (3,98), (4,97); sum of i+j = 101+101+101+101 = 404.
    if (auto v = zip_take_sum(); v != 404) {
        std::println("zip_take_sum expected 404, got {}", v);
        return EXIT_FAILURE;
    }

    std::println("iter_combinators OK");
    return EXIT_SUCCESS;
}
