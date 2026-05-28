// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#include "map_error.hpp"

#include <cstdlib>
#include <print>

int main() {
    using namespace examples::map_error;

    // Success path: parseInt(7) returns Int32, mapError unused.
    auto ok = parseOrWrap(7);
    if (!ok.has_value() || ok.value() != 7) {
        std::println("parseOrWrap(7) wrong");
        return EXIT_FAILURE;
    }

    // Negative input: parseInt throws ParseErr.negative → toAppErr →
    // AppErr.badInput at the outer surface.
    auto neg = parseOrWrap(-1);
    if (neg.has_value() || neg.error() != AppErr::badInput) {
        std::println("parseOrWrap(-1) should be AppErr.badInput");
        return EXIT_FAILURE;
    }

    // Overflow input: parseInt throws ParseErr.overflow → toAppErr →
    // AppErr.internalFailure.
    auto over = parseOrWrap(9999);
    if (over.has_value() || over.error() != AppErr::internalFailure) {
        std::println("parseOrWrap(9999) should be AppErr.internalFailure");
        return EXIT_FAILURE;
    }

    // Closure-literal form: same mapping policy as `toAppErr` but
    // inlined at the call site. Verifies sema pushes the expected
    // `(ParseErr) -> ?` Function type into the closure so `pe`
    // adopts ParseErr.
    auto inline_ok = parseInlineWrap(42);
    if (!inline_ok.has_value() || inline_ok.value() != 42) {
        std::println("parseInlineWrap(42) wrong");
        return EXIT_FAILURE;
    }
    auto inline_neg = parseInlineWrap(-5);
    if (inline_neg.has_value() || inline_neg.error() != AppErr::badInput) {
        std::println("parseInlineWrap(-5) should be AppErr.badInput");
        return EXIT_FAILURE;
    }
    auto inline_over = parseInlineWrap(2000);
    if (inline_over.has_value() || inline_over.error() != AppErr::internalFailure) {
        std::println("parseInlineWrap(2000) should be AppErr.internalFailure");
        return EXIT_FAILURE;
    }

    std::println("mapError OK");
    return EXIT_SUCCESS;
}
