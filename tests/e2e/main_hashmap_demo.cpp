// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

// E2E driver for §18.5 HashMap: an owned hash map (→ std::unordered_map).
// Exercises HashMap.new() + set (insert-or-assign) + get (V?) + contains + len.

#include "hashmap_demo.hpp"

#include <cassert>
#include <cstdio>
#include <string_view>

namespace hd = examples::hashmap_demo;

int main() {
    // "apple" was set twice; set() is insert-or-assign, so only 2 distinct keys.
    assert(hd::distinctWords() == 2);
    // get() yields the latest value for a present key, through `??`.
    assert(hd::countOr(std::string_view("apple"), -1) == 3);
    assert(hd::countOr(std::string_view("pear"), -1) == 2);
    // Absent key falls back to the default.
    assert(hd::countOr(std::string_view("missing"), -1) == -1);
    assert(hd::hasWord(std::string_view("pear")));
    assert(!hd::hasWord(std::string_view("missing")));
    std::puts("hashmap_demo OK");
    return 0;
}
