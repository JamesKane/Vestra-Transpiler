// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

// E2E driver for §18.5 String/Str read-only queries. Exercises the byte-oriented
// slicing and search surface (len / isEmpty / slice / byteAt / find / contains /
// startsWith / endsWith) over borrowed Str views and one owned String receiver,
// including the clamped/out-of-range edge cases.

#include "string_query_demo.hpp"

#include <cassert>
#include <cstdio>
#include <string_view>

namespace sq = examples::string_query_demo;

int main() {
    using std::string_view;

    // len / isEmpty.
    assert(sq::length(string_view("hello")) == 5);
    assert(sq::length(string_view("")) == 0);
    assert(sq::empty(string_view("")));
    assert(!sq::empty(string_view("x")));

    // slice: the [from, to) sub-view, with clamping on the edges.
    assert(sq::substr(string_view("hello world"), 0, 5) == "hello");
    assert(sq::substr(string_view("hello world"), 6, 11) == "world");
    // out-of-range high bound clamps to the end.
    assert(sq::substr(string_view("hi"), 0, 99) == "hi");
    // inverted span yields empty.
    assert(sq::substr(string_view("hi"), 5, 1).empty());
    // negative from clamps to 0.
    assert(sq::substr(string_view("abc"), -3, 2) == "ab");

    // byteAt via ?? default: in range returns the byte, out of range the default.
    assert(sq::byteOr(string_view("ABC"), 0, 0) == static_cast<std::uint8_t>('A'));
    assert(sq::byteOr(string_view("ABC"), 2, 0) == static_cast<std::uint8_t>('C'));
    assert(sq::byteOr(string_view("ABC"), 9, 255) == 255);
    assert(sq::byteOr(string_view("ABC"), -1, 255) == 255);

    // find via if-let, -1 sentinel when absent.
    assert(sq::foundAt(string_view("hello world"), string_view("world")) == 6);
    assert(sq::foundAt(string_view("hello world"), string_view("xyz")) == -1);
    assert(sq::foundAt(string_view("aXbXc"), string_view("X")) == 1);  // first occurrence

    // contains / startsWith / endsWith.
    assert(sq::has(string_view("hello world"), string_view("o w")));
    assert(!sq::has(string_view("hello"), string_view("z")));
    assert(sq::prefixed(string_view("foobar"), string_view("foo")));
    assert(!sq::prefixed(string_view("foobar"), string_view("bar")));
    assert(sq::suffixed(string_view("foobar"), string_view("bar")));
    assert(!sq::suffixed(string_view("foobar"), string_view("foo")));

    // afterDot: slice off everything past the first dot; whole string if no dot.
    assert(sq::afterDot(string_view("main.vst")) == "vst");
    assert(sq::afterDot(string_view("a.b.c")) == "b.c");  // first dot only
    assert(sq::afterDot(string_view("nodot")) == "nodot");

    // The same query surface on an owned String receiver ("Hello, Bob!").
    assert(sq::builtContains(string_view("Bob"), string_view("Bob")));
    assert(sq::builtContains(string_view("Bob"), string_view("Hello")));
    assert(!sq::builtContains(string_view("Bob"), string_view("zzz")));

    std::puts("string_query_demo OK");
    return 0;
}
