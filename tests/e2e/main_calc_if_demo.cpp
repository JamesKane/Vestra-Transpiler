// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

// E2E driver for the conditionals mini-language (examples/calc_if_demo.vst).
// Checks comparisons, if/then/else selection, nesting, interaction with the
// variable environment (abs, max), and that division is total (x / 0 = 0) so the
// untaken branch can be evaluated harmlessly.

#include "calc_if_demo.hpp"

#include <cassert>
#include <cstdio>
#include <string_view>

namespace ci = examples::calc_if_demo;

int main() {
    using std::string_view;

    // Comparisons yield 1 / 0.
    assert(ci::runProgram(string_view("3 < 5")) == 1);
    assert(ci::runProgram(string_view("5 < 3")) == 0);
    assert(ci::runProgram(string_view("7 > 2")) == 1);

    // Basic if/then/else selection.
    assert(ci::runProgram(string_view("if 1 then 10 else 20")) == 10);
    assert(ci::runProgram(string_view("if 0 then 10 else 20")) == 20);

    // Condition over a comparison and a variable.
    assert(ci::runProgram(string_view("x = 5; if x > 3 then 100 else 0")) == 100);
    assert(ci::runProgram(string_view("x = 2; if x > 3 then 100 else 0")) == 0);

    // max(a, b) via a conditional over two variables.
    assert(ci::runProgram(string_view("a = 4; b = 7; if a < b then b else a")) == 7);
    assert(ci::runProgram(string_view("a = 9; b = 7; if a < b then b else a")) == 9);

    // abs(n): negate when negative.
    assert(ci::runProgram(string_view("n = 0 - 7; if n < 0 then 0 - n else n")) == 7);
    assert(ci::runProgram(string_view("n = 4; if n < 0 then 0 - n else n")) == 4);

    // Nested if in the else branch.
    assert(ci::runProgram(string_view("if 2 < 1 then 99 else if 3 > 2 then 42 else 0")) == 42);

    // Division is total, so the untaken `10 / 0` branch is harmless.
    assert(ci::runProgram(string_view("if 1 then 6 else 10 / 0")) == 6);
    assert(ci::runProgram(string_view("10 / 0")) == 0);

    std::puts("calc_if_demo OK");
    return 0;
}
