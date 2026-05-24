// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#include "forin.hpp"

#include <cstdlib>
#include <print>

int main() {
    using namespace examples::forin;

    if (sum_inclusive() != 55) {
        std::println("sum_inclusive wrong: {}", sum_inclusive());
        return EXIT_FAILURE;
    }
    if (sum_exclusive() != 45) {
        std::println("sum_exclusive wrong: {}", sum_exclusive());
        return EXIT_FAILURE;
    }
    if (sum_countdown(5) != 15) {
        std::println("sum_countdown(5) wrong: {}", sum_countdown(5));
        return EXIT_FAILURE;
    }
    if (sum_countdown(0) != 0) {
        std::println("sum_countdown(0) wrong: {}", sum_countdown(0));
        return EXIT_FAILURE;
    }

    std::println("forin OK");
    return EXIT_SUCCESS;
}
