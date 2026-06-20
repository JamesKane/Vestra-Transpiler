// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

// E2E driver for the tagged value type (examples/calc_value_demo.vst). Each
// program is evaluated to a Value and rendered by `format`, so the expected
// string also pins the *type* of the result: "14" (int), "true" (bool), "yes"
// (str). Covers arithmetic, comparison, structural / cross-type equality, string
// and boolean literals, and `if` choosing between branches of different types.

#include "calc_value_demo.hpp"

#include <cassert>
#include <cstdio>
#include <string_view>

namespace cv = examples::calc_value_demo;

int main() {
    using std::string_view;

    // Arithmetic stays integer-typed.
    assert(cv::evalStr(string_view("2 + 3 * 4")) == "14");
    assert(cv::evalStr(string_view("(2 + 3) * 4")) == "20");

    // Comparison and equality produce booleans.
    assert(cv::evalStr(string_view("3 < 5")) == "true");
    assert(cv::evalStr(string_view("5 < 3")) == "false");
    assert(cv::evalStr(string_view("7 > 2")) == "true");
    assert(cv::evalStr(string_view("2 == 2")) == "true");
    assert(cv::evalStr(string_view("2 == 3")) == "false");

    // Boolean literals.
    assert(cv::evalStr(string_view("true")) == "true");
    assert(cv::evalStr(string_view("false")) == "false");

    // String literals and string equality.
    assert(cv::evalStr(string_view("\"hello\"")) == "hello");
    assert(cv::evalStr(string_view("\"abc\" == \"abc\"")) == "true");
    assert(cv::evalStr(string_view("\"abc\" == \"abd\"")) == "false");

    // Cross-type equality is never true.
    assert(cv::evalStr(string_view("1 == \"x\"")) == "false");
    assert(cv::evalStr(string_view("true == 1")) == "false");

    // `if` selects a branch; the result keeps its branch's type.
    assert(cv::evalStr(string_view("if 1 < 2 then 10 else 20")) == "10");
    assert(cv::evalStr(string_view("if 5 < 2 then 10 else 20")) == "20");
    assert(cv::evalStr(string_view("if true then 1 else 0")) == "1");

    // The highlight: branches of different value types.
    assert(cv::evalStr(string_view("if 1 < 2 then \"small\" else 99")) == "small");
    assert(cv::evalStr(string_view("if 9 < 2 then \"small\" else 99")) == "99");
    assert(cv::evalStr(string_view("if \"a\" == \"a\" then \"eq\" else \"ne\"")) == "eq");

    std::puts("calc_value_demo OK");
    return 0;
}
