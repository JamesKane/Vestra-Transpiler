// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

// E2E driver for the variable-binding mini-language (examples/calc_vars_demo.vst).
// Exercises the HashMap symbol table: binding, reference in later statements,
// reassignment (reading the old value), multi-statement programs, and undefined
// names defaulting to 0.

#include "calc_vars_demo.hpp"

#include <cassert>
#include <cstdio>
#include <string_view>

namespace cv = examples::calc_vars_demo;

int main() {
    using std::string_view;

    // Bind a variable, then reference it.
    assert(cv::runProgram(string_view("x = 5; x + 3")) == 8);

    // Several bindings used together.
    assert(cv::runProgram(string_view("a = 2; b = 3; a * b")) == 6);

    // A binding that references an earlier one.
    assert(cv::runProgram(string_view("x = 10; y = x * 2; x + y")) == 30);

    // Reassignment reads the old value of the same name.
    assert(cv::runProgram(string_view("x = 1; x = x + 1; x = x + 1; x")) == 3);

    // Parentheses and precedence still hold inside a bound expression.
    assert(cv::runProgram(string_view("w = (2 + 3) * 4; w - 2")) == 18);

    // No variables at all — just an expression.
    assert(cv::runProgram(string_view("5 + 3")) == 8);

    // Multi-letter identifiers, and the last statement is the result.
    assert(cv::runProgram(string_view("width = 4; height = 5; width * height")) == 20);

    // An undefined name reads as 0; an empty program is 0.
    assert(cv::runProgram(string_view("foo")) == 0);
    assert(cv::runProgram(string_view("")) == 0);
    assert(cv::runProgram(string_view("undefined + 7")) == 7);

    std::puts("calc_vars_demo OK");
    return 0;
}
