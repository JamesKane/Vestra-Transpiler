// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#include "result.hpp"

#include <cstdlib>
#include <expected>
#include <print>

int main() {
    using namespace examples::result;

    // Success path on the fallible primitive.
    auto ok = divide(10, 2);
    if (!ok.has_value() || *ok != 5) {
        std::println("divide(10, 2) wrong: has_value={}", ok.has_value());
        return EXIT_FAILURE;
    }

    // Failure path — divByZero arm.
    auto err = divide(10, 0);
    if (err.has_value() || err.error() != DivErr::divByZero) {
        std::println("divide(10, 0) should be divByZero");
        return EXIT_FAILURE;
    }

    // try? → Optional → ?? default.
    if (safe_div(10, 2) != 5) {
        std::println("safe_div ok arm wrong");
        return EXIT_FAILURE;
    }
    if (safe_div(10, 0) != 0) {
        std::println("safe_div err arm wrong");
        return EXIT_FAILURE;
    }

    // try! success.
    if (unwrap_div(10, 5) != 2) {
        std::println("unwrap_div ok arm wrong");
        return EXIT_FAILURE;
    }

    // try! failure → panic (std::bad_expected_access).
    try {
        (void)unwrap_div(10, 0);
        std::println("unwrap_div(10, 0) should have panicked");
        return EXIT_FAILURE;
    } catch (const std::bad_expected_access<DivErr>&) {
        // expected
    }

    // Propagating chain: every divide succeeds.
    auto c1 = chain(20, 2, 5);
    if (!c1.has_value() || *c1 != 2) {
        std::println("chain(20, 2, 5) wrong");
        return EXIT_FAILURE;
    }

    // Propagating chain: the second divide explodes — error flows up.
    auto c2 = chain(20, 2, 0);
    if (c2.has_value() || c2.error() != DivErr::divByZero) {
        std::println("chain(20, 2, 0) should propagate divByZero");
        return EXIT_FAILURE;
    }

    // Propagating chain: the first divide explodes — error flows up too.
    auto c3 = chain(20, 0, 5);
    if (c3.has_value() || c3.error() != DivErr::divByZero) {
        std::println("chain(20, 0, 5) should propagate divByZero");
        return EXIT_FAILURE;
    }

    // Mid-expression try: both divides succeed → sum.
    auto s1 = sum_quotients(10, 2, 9, 3);
    if (!s1.has_value() || *s1 != 8) {
        std::println("sum_quotients(10, 2, 9, 3) wrong");
        return EXIT_FAILURE;
    }
    // Mid-expression try: first divide errors → propagate, second
    // doesn't run (so no spurious side-effect).
    auto s2 = sum_quotients(10, 0, 9, 3);
    if (s2.has_value() || s2.error() != DivErr::divByZero) {
        std::println("sum_quotients(10, 0, ...) should propagate");
        return EXIT_FAILURE;
    }
    // Mid-expression try: second divide errors → also propagates.
    auto s3 = sum_quotients(10, 2, 9, 0);
    if (s3.has_value() || s3.error() != DivErr::divByZero) {
        std::println("sum_quotients(..., 9, 0) should propagate");
        return EXIT_FAILURE;
    }

    std::println("result OK");
    return EXIT_SUCCESS;
}
