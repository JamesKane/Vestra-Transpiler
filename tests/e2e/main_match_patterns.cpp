// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#include "match_patterns.hpp"

#include <cstdlib>
#include <print>
#include <tuple>

int main() {
    using namespace examples::match_patterns;

    // --- bare-enum or-pattern ---
    if (isWarm(Color::red) != 1 || isWarm(Color::green) != 1 || isWarm(Color::yellow) != 1) {
        std::println("warm path wrong");
        return EXIT_FAILURE;
    }
    if (isWarm(Color::blue) != 0) {
        std::println("blue should be cool");
        return EXIT_FAILURE;
    }

    // --- integer scrutinee: literal + or + range ---
    if (bucket(0) != 0)
        return EXIT_FAILURE;
    if (bucket(1) != 1 || bucket(2) != 1 || bucket(3) != 1)
        return EXIT_FAILURE;
    if (bucket(4) != 2 || bucket(9) != 2)
        return EXIT_FAILURE;
    if (bucket(10) != 3 || bucket(20) != 3)
        return EXIT_FAILURE;
    if (bucket(21) != -1 || bucket(-5) != -1)
        return EXIT_FAILURE;

    // --- exclusive vs inclusive range boundaries ---
    if (grade(59) != 0)
        return EXIT_FAILURE;
    if (grade(60) != 1 || grade(69) != 1)
        return EXIT_FAILURE;
    if (grade(70) != 2 || grade(79) != 2)
        return EXIT_FAILURE;
    if (grade(89) != 3)
        return EXIT_FAILURE;
    if (grade(90) != 4 || grade(100) != 4)
        return EXIT_FAILURE;  // inclusive of 100
    if (grade(101) != -1)
        return EXIT_FAILURE;

    // --- tuple-scrutinee match ---
    if (quadrant(std::tuple<std::int32_t, std::int32_t>{0, 0}) != 0)
        return EXIT_FAILURE;
    if (quadrant(std::tuple<std::int32_t, std::int32_t>{0, 7}) != 1)
        return EXIT_FAILURE;
    if (quadrant(std::tuple<std::int32_t, std::int32_t>{7, 0}) != 2)
        return EXIT_FAILURE;
    if (quadrant(std::tuple<std::int32_t, std::int32_t>{3, 4}) != 3)
        return EXIT_FAILURE;

    std::println("match patterns OK");
    return EXIT_SUCCESS;
}
