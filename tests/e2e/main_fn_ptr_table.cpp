// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#include "fn_ptr_table.hpp"

#include <cstdint>
#include <cstdlib>
#include <print>

int main() {
    using namespace examples::fn_ptr_table;

    // pick(idx, arg) routes through handler_a / handler_b / handler_c,
    // each of which adds 10 / 100 / 1000 to its argument. Direct
    // dispatch tests the function-pointer let-binding path.
    if (auto v = pick(0, 5); v != 15) {
        std::println("pick(0, 5) expected 15, got {}", v);
        return EXIT_FAILURE;
    }
    if (auto v = pick(1, 5); v != 105) {
        std::println("pick(1, 5) expected 105, got {}", v);
        return EXIT_FAILURE;
    }
    if (auto v = pick(2, 5); v != 1005) {
        std::println("pick(2, 5) expected 1005, got {}", v);
        return EXIT_FAILURE;
    }

    // apply(handler_b, arg) tests the function-pointer-typed
    // parameter path; the bare `handler_b` decays to its function
    // pointer at the C++ call site.
    if (auto v = apply(handler_b, 7); v != 107) {
        std::println("apply(handler_b, 7) expected 107, got {}", v);
        return EXIT_FAILURE;
    }

    // drive combines both: pick(0, 3) = 13, apply(handler_b, 3) = 103;
    // total 116.
    if (auto v = drive(0, 3); v != 116) {
        std::println("drive(0, 3) expected 116, got {}", v);
        return EXIT_FAILURE;
    }

    std::println("fn_ptr_table OK");
    return EXIT_SUCCESS;
}
