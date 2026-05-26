// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#include "midtry.hpp"

#include <cstdlib>
#include <print>

int main() {
    using namespace examples::midtry;

    // choose_return: both branches succeed → unwrap to the picked value.
    if (choose_return(true, 7, -1).value() != 7) {
        std::println("choose_return(true, 7) wrong");
        return EXIT_FAILURE;
    }
    if (choose_return(false, -1, 9).value() != 9) {
        std::println("choose_return(false, 9) wrong");
        return EXIT_FAILURE;
    }
    // choose_return: picked branch fails → error propagates.
    if (choose_return(true, -5, 9).has_value()) {
        std::println("choose_return error path leaked the error");
        return EXIT_FAILURE;
    }

    // choose_let: same shape as choose_return but via a LetStmt.
    if (choose_let(true, 3, 0).value() != 3) {
        std::println("choose_let happy path wrong");
        return EXIT_FAILURE;
    }
    if (choose_let(false, 0, -2).has_value()) {
        std::println("choose_let error path leaked");
        return EXIT_FAILURE;
    }

    // plus_one: the if-expr is a sub-expression of a +1.
    if (plus_one(true, 10, 0).value() != 11) {
        std::println("plus_one happy path wrong");
        return EXIT_FAILURE;
    }
    if (plus_one(false, 0, -7).has_value()) {
        std::println("plus_one error path leaked");
        return EXIT_FAILURE;
    }

    // with_bump: sub-expression try inside one branch body.
    if (with_bump(true, 5, 99).value() != 6) {
        std::println("with_bump happy path wrong");
        return EXIT_FAILURE;
    }
    if (with_bump(true, -5, 99).has_value()) {
        std::println("with_bump error path leaked");
        return EXIT_FAILURE;
    }
    // Fallback branch is reached when c is false — no try fires.
    if (with_bump(false, -5, 42).value() != 42) {
        std::println("with_bump fallback wrong");
        return EXIT_FAILURE;
    }

    std::println("midtry OK");
    return EXIT_SUCCESS;
}
