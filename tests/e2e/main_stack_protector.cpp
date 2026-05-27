// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#include "stack_protector.hpp"

#include <cstdint>
#include <cstdlib>
#include <print>

int main() {
    using namespace examples::stack_protector;

    // Behavior of the two functions is unaffected by the
    // attribute — the canary is a separate runtime check. The
    // e2e proves both lower correctly and run; the
    // [[gnu::no_stack_protector]] emission is asserted at
    // codegen-test time.
    if (auto v = critical_path(21); v != 42) {
        std::println("critical_path(21) expected 42, got {}", v);
        return EXIT_FAILURE;
    }
    if (auto v = careful_path(5); v != 105) {
        std::println("careful_path(5) expected 105, got {}", v);
        return EXIT_FAILURE;
    }

    std::println("stack_protector OK");
    return EXIT_SUCCESS;
}
