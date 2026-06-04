// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

// E2E driver for §18.5 String: an owned growable string (→ std::string).
// Exercises String.new() + append (literal and borrowed Str) + len().

#include "string_demo.hpp"

#include <cassert>
#include <cstdio>
#include <string_view>

namespace sd = examples::string_demo;

int main() {
    assert(sd::greeting(std::string_view("Bob")) == "Hello, Bob!");
    assert(sd::greetingLen(std::string_view("Bob")) == 11);
    std::puts("string_demo OK");
    return 0;
}
