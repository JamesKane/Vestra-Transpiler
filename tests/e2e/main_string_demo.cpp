// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

// E2E driver for §18.5 String: an owned growable string (→ std::string).
// Exercises String.new() + append (literal, borrowed Str, and an owned String
// read-borrowed as a Str) + len().

#include "string_demo.hpp"

#include <cassert>
#include <cstdio>
#include <string_view>

namespace sd = examples::string_demo;

int main() {
    assert(sd::greeting(std::string_view("Bob")) == "Hello, Bob!");
    assert(sd::greetingLen(std::string_view("Bob")) == 11);
    // "abc" + "defg" appended owned-into-owned -> 7 bytes.
    assert(sd::joined(std::string_view("abc"), std::string_view("defg")) == 7);
    std::puts("string_demo OK");
    return 0;
}
