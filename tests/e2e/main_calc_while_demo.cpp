// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

// E2E driver for while loops (examples/calc_while_demo.vst). Exercises iterative
// sum / factorial / power, a single-statement body, a loop that runs zero times,
// iterative Fibonacci, a loop calling a function, and nested loops — each
// mutating a shared environment until the condition flips.

#include "calc_while_demo.hpp"

#include <cassert>
#include <cstdio>
#include <string_view>

namespace cw = examples::calc_while_demo;

int main() {
    using std::string_view;

    // Sum 1..10.
    assert(cw::runProgram(string_view("s = 0; i = 1; while i < 11 do { s = s + i; i = i + 1 }; s"))
           == 55);

    // Iterative factorial: 5!.
    assert(cw::runProgram(string_view("f = 1; i = 1; while i < 6 do { f = f * i; i = i + 1 }; f"))
           == 120);

    // Iterative power: 2^10.
    assert(cw::runProgram(string_view("r = 1; e = 10; while e > 0 do { r = r * 2; e = e - 1 }; r"))
           == 1024);

    // A single-statement body (no braces).
    assert(cw::runProgram(string_view("i = 0; while i < 5 do i = i + 1; i")) == 5);

    // A loop whose condition is false up front runs zero times.
    assert(cw::runProgram(string_view("x = 5; while x > 10 do x = x + 1; x")) == 5);

    // Iterative Fibonacci: fib(10).
    assert(cw::runProgram(string_view(
               "a = 0; b = 1; n = 10; while n > 0 do { t = a + b; a = b; b = t; n = n - 1 }; a"))
           == 55);

    // A loop that calls a function each iteration: sum of squares 1..3.
    assert(cw::runProgram(string_view(
               "fn sq(x) = x * x; s = 0; i = 1; while i < 4 do { s = s + sq(i); i = i + 1 }; s"))
           == 14);

    // Nested loops: 3 * 4 increments.
    assert(cw::runProgram(string_view(
               "p = 0; i = 0; while i < 3 do { j = 0; while j < 4 do { p = p + 1; j = j + 1 }; "
               "i = i + 1 }; p"))
           == 12);

    // Conditionals and functions still work alongside loops.
    assert(cw::runProgram(string_view("fn max(a, b) = if a < b then b else a; max(3, 7)")) == 7);

    std::puts("calc_while_demo OK");
    return 0;
}
