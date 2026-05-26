// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#include "nested_tuples.hpp"

#include <cstdlib>
#include <print>
#include <tuple>

int main() {
    using namespace examples::nested_tuples;

    // summarise(2, 5, 4) → ((11, 3), 5). The outer second is the max.
    auto s = summarise(2, 5, 4);
    if (std::get<1>(s) != 5) {
        std::println("summarise peak wrong: {}", std::get<1>(s));
        return EXIT_FAILURE;
    }
    if (std::get<0>(std::get<0>(s)) != 11 || std::get<1>(std::get<0>(s)) != 3) {
        std::println("summarise inner wrong");
        return EXIT_FAILURE;
    }

    // mean_and_peak: (11 / 3, 5) → (3, 5). Inner sub-tuple bindings
    // are read off the nested pattern's follow-on `auto [sum, count]
    // = ...;` statement.
    auto mp = mean_and_peak(2, 5, 4);
    if (std::get<0>(mp) != 3 || std::get<1>(mp) != 5) {
        std::println("mean_and_peak wrong: ({}, {})", std::get<0>(mp), std::get<1>(mp));
        return EXIT_FAILURE;
    }

    // bump_sum: `var` at the inner level, sum gets bumped before the
    // final sum-of-everything. 12 + 3 + 5 = 20.
    if (bump_sum(2, 5, 4) != 20) {
        std::println("bump_sum wrong: {}", bump_sum(2, 5, 4));
        return EXIT_FAILURE;
    }

    // sum_and_peak: inner wildcard drops the count slot. 11 + 5 = 16.
    if (sum_and_peak(2, 5, 4) != 16) {
        std::println("sum_and_peak wrong: {}", sum_and_peak(2, 5, 4));
        return EXIT_FAILURE;
    }

    // Three-level nesting: 1 + 2 + 3 + 4 = 10.
    if (triple_sum() != 10) {
        std::println("triple_sum wrong: {}", triple_sum());
        return EXIT_FAILURE;
    }

    std::println("nested tuples OK");
    return EXIT_SUCCESS;
}
