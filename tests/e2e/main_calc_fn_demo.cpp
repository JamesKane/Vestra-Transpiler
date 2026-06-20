// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

// E2E driver for user-defined functions (examples/calc_fn_demo.vst). Exercises
// definition + call, arguments, nested calls, interaction with variables, and —
// the headline — recursion (factorial, Fibonacci), which only terminates because
// the conditional is lazy (the untaken branch fires no recursive call).

#include "calc_fn_demo.hpp"

#include <cassert>
#include <cstdio>
#include <string_view>

namespace fn = examples::calc_fn_demo;

int main() {
    using std::string_view;

    // Define and call a simple function.
    assert(fn::runProgram(string_view("fn sq(x) = x * x; sq(5)")) == 25);
    assert(fn::runProgram(string_view("fn dbl(x) = x + x; dbl(7)")) == 14);

    // The argument is an expression; the call sits inside one too.
    assert(fn::runProgram(string_view("fn sq(x) = x * x; sq(2 + 3)")) == 25);
    assert(fn::runProgram(string_view("fn sq(x) = x * x; sq(3) + sq(4)")) == 25);

    // Calls interact with the variable environment.
    assert(fn::runProgram(string_view("fn sq(x) = x * x; a = 6; sq(a)")) == 36);

    // Nested calls.
    assert(fn::runProgram(string_view("fn inc(x) = x + 1; inc(inc(inc(0)))")) == 3);

    // Recursion: factorial.
    assert(fn::runProgram(string_view("fn fact(n) = if n < 2 then 1 else n * fact(n - 1); fact(0)"))
           == 1);
    assert(fn::runProgram(string_view("fn fact(n) = if n < 2 then 1 else n * fact(n - 1); fact(1)"))
           == 1);
    assert(fn::runProgram(string_view("fn fact(n) = if n < 2 then 1 else n * fact(n - 1); fact(5)"))
           == 120);
    assert(fn::runProgram(string_view("fn fact(n) = if n < 2 then 1 else n * fact(n - 1); fact(6)"))
           == 720);

    // Recursion: Fibonacci (0,1,1,2,3,5,8,13,21,34,55).
    assert(fn::runProgram(
               string_view("fn fib(n) = if n < 2 then n else fib(n - 1) + fib(n - 2); fib(7)"))
           == 13);
    assert(fn::runProgram(
               string_view("fn fib(n) = if n < 2 then n else fib(n - 1) + fib(n - 2); fib(10)"))
           == 55);

    std::puts("calc_fn_demo OK");
    return 0;
}
