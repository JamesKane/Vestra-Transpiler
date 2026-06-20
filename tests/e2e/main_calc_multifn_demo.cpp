// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

// E2E driver for multi-argument functions (examples/calc_multifn_demo.vst).
// Exercises 2- and 3-parameter functions, positional argument binding, argument
// expressions and nested calls, two-argument recursion (power, gcd), and that
// zero- and one-argument functions still work.

#include "calc_multifn_demo.hpp"

#include <cassert>
#include <cstdio>
#include <string_view>

namespace mf = examples::calc_multifn_demo;

int main() {
    using std::string_view;

    // Two-argument functions.
    assert(mf::runProgram(string_view("fn add(a, b) = a + b; add(3, 4)")) == 7);
    assert(mf::runProgram(string_view("fn mul(a, b) = a * b; mul(6, 7)")) == 42);

    // Arguments are expressions; order is positional.
    assert(mf::runProgram(string_view("fn sub(a, b) = a - b; sub(10, 2 + 1)")) == 7);

    // A conditional over two parameters: max.
    assert(mf::runProgram(string_view("fn max(a, b) = if a < b then b else a; max(3, 7)")) == 7);
    assert(mf::runProgram(string_view("fn max(a, b) = if a < b then b else a; max(9, 2)")) == 9);

    // Three parameters.
    assert(mf::runProgram(string_view("fn sum3(a, b, c) = a + b + c; sum3(1, 2, 3)")) == 6);

    // Nested calls across two functions.
    assert(mf::runProgram(string_view("fn sq(x) = x * x; fn add(a, b) = a + b; add(sq(2), sq(3))"))
           == 13);

    // Two-argument recursion: power.
    assert(mf::runProgram(
               string_view("fn pow(b, e) = if e < 1 then 1 else b * pow(b, e - 1); pow(2, 10)"))
           == 1024);
    assert(mf::runProgram(
               string_view("fn pow(b, e) = if e < 1 then 1 else b * pow(b, e - 1); pow(3, 4)"))
           == 81);

    // Two-argument recursion with division: Euclid's gcd.
    assert(mf::runProgram(string_view(
               "fn gcd(a, b) = if b < 1 then a else gcd(b, a - (a / b) * b); gcd(48, 36)"))
           == 12);
    assert(mf::runProgram(string_view(
               "fn gcd(a, b) = if b < 1 then a else gcd(b, a - (a / b) * b); gcd(17, 5)"))
           == 1);

    // One- and zero-argument functions still work.
    assert(mf::runProgram(string_view("fn sq(x) = x * x; sq(5)")) == 25);
    assert(mf::runProgram(string_view("fn five() = 5; five() + 1")) == 6);

    std::puts("calc_multifn_demo OK");
    return 0;
}
