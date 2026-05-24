// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#include "docatch.hpp"

#include <cstdlib>
#include <print>

int main() {
    using namespace examples::docatch;

    // Success path — do-body yields the value.
    if (safe_parse(7) != 7) {
        std::println("safe_parse(7) wrong");
        return EXIT_FAILURE;
    }
    // Error path — catch arm fires.
    if (safe_parse(-3) != -1) {
        std::println("safe_parse(-3) wrong");
        return EXIT_FAILURE;
    }

    // Multi-try body: both ok.
    if (sum_two(2, 3) != 5) {
        std::println("sum_two(2, 3) wrong");
        return EXIT_FAILURE;
    }
    // Multi-try body: first fails → catch.
    if (sum_two(-1, 3) != 0) {
        std::println("sum_two(-1, 3) wrong");
        return EXIT_FAILURE;
    }
    // Multi-try body: second fails → catch.
    if (sum_two(2, -1) != 0) {
        std::println("sum_two(2, -1) wrong");
        return EXIT_FAILURE;
    }

    // Tagged catch uses the bound error value.
    if (tagged(5) != 5) {
        std::println("tagged(5) wrong");
        return EXIT_FAILURE;
    }
    if (tagged(-1) != -100) {
        std::println("tagged(-1) wrong");
        return EXIT_FAILURE;
    }

    // Bare `catch NAME` form — sema-inferred error type.
    if (safe_parse_bare(7) != 7) {
        std::println("safe_parse_bare(7) wrong");
        return EXIT_FAILURE;
    }
    if (safe_parse_bare(-3) != -1) {
        std::println("safe_parse_bare(-3) wrong");
        return EXIT_FAILURE;
    }

    std::println("docatch OK");
    return EXIT_SUCCESS;
}
