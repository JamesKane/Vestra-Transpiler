// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#include "optional.hpp"

#include <cstdlib>
#include <optional>
#include <print>

int main() {
    using namespace examples::optional;

    const auto e = empty();
    if (e.has_value()) {
        std::println("empty() must be std::nullopt, got {}", *e);
        return EXIT_FAILURE;
    }

    const auto f = filled(7);
    if (!f.has_value() || *f != 7) {
        std::println("filled(7) must be 7, got {}", f.value_or(-1));
        return EXIT_FAILURE;
    }

    // ?? returns wrapped value when present, default when absent.
    if (coalesce(filled(11), 0) != 11) {
        std::println("?? present arm wrong");
        return EXIT_FAILURE;
    }
    if (coalesce(empty(), 42) != 42) {
        std::println("?? absent arm wrong");
        return EXIT_FAILURE;
    }

    // if let runs the then-branch when present, else-branch when absent.
    if (double_or_zero(filled(5)) != 10) {
        std::println("if let then-branch wrong");
        return EXIT_FAILURE;
    }
    if (double_or_zero(empty()) != 0) {
        std::println("if let else-branch wrong");
        return EXIT_FAILURE;
    }

    // Force-unwrap on a `.some` yields the wrapped value.
    if (forced(filled(99)) != 99) {
        std::println("force-unwrap on some wrong");
        return EXIT_FAILURE;
    }

    // Force-unwrap on `.none` panics via `__vstr::panic` → std::abort.
    // We can't catch the abort in-process, so this driver only
    // exercises the .some path; the nil-panic behavior is covered by
    // the e2e_panic driver (which forks and asserts the child aborts).

    std::println("optional OK");
    return EXIT_SUCCESS;
}
