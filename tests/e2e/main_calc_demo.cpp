// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

// E2E driver for the self-hosting recursive-descent parser (examples/calc_demo.vst).
// Checks operator precedence, left-associativity, parentheses, nesting, and
// whitespace handling — the things a real expression grammar must get right.

#include "calc_demo.hpp"

#include <cassert>
#include <cstdio>
#include <string_view>

namespace cd = examples::calc_demo;

int main() {
    using std::string_view;

    // Atoms and whitespace.
    assert(cd::eval(string_view("42")) == 42);
    assert(cd::eval(string_view("  7  ")) == 7);

    // Left-to-right within a precedence level.
    assert(cd::eval(string_view("1 + 2 + 3")) == 6);
    assert(cd::eval(string_view("10 - 3 - 2")) == 5);  // (10-3)-2, not 10-(3-2)
    assert(cd::eval(string_view("20 / 2 / 5")) == 2);  // (20/2)/5

    // Precedence: * and / bind tighter than + and -.
    assert(cd::eval(string_view("2 + 3 * 4")) == 14);
    assert(cd::eval(string_view("2 * 3 + 4")) == 10);
    assert(cd::eval(string_view("100 - 2 * 30")) == 40);
    assert(cd::eval(string_view("12 / 3 + 1")) == 5);

    // Parentheses override precedence.
    assert(cd::eval(string_view("(2 + 3) * 4")) == 20);
    assert(cd::eval(string_view("2 * (3 + 4)")) == 14);
    assert(cd::eval(string_view("(1 + 2) * (3 + 4)")) == 21);

    // Nesting / deeper recursion.
    assert(cd::eval(string_view("((1 + 2) * 3) - 4")) == 5);
    assert(cd::eval(string_view("2 * (3 + (4 * 5))")) == 46);
    assert(cd::eval(string_view("((((10))))")) == 10);

    // A larger mixed expression.
    assert(cd::eval(string_view("1 + 2 * 3 - 4 / 2 + (5 - 1) * 2")) == 13);

    std::puts("calc_demo OK");
    return 0;
}
