// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

// E2E driver for §18.5 number<->string conversion. Exercises n.toString()
// rendering across numeric primitives and Str.toInt() parsing (including the
// malformed/partial-input nil cases, the ?? and if-let consumers, and a
// render->parse round-trip).

#include "string_convert_demo.hpp"

#include <cassert>
#include <cstdio>
#include <string_view>

namespace sc = examples::string_convert_demo;

int main() {
    using std::string_view;

    // toString rendering.
    assert(sc::render(0) == "0");
    assert(sc::render(42) == "42");
    assert(sc::render(-7) == "-7");
    assert(sc::renderI32(123) == "123");
    assert(sc::renderI32(-123) == "-123");
    // Float renders via std::format's shortest round-trip.
    assert(sc::renderF64(1.5) == "1.5");
    assert(sc::renderF64(-0.25) == "-0.25");

    // toInt parsing: well-formed -> value, malformed/partial -> default.
    assert(sc::parseOr(string_view("42"), -1) == 42);
    assert(sc::parseOr(string_view("-7"), -1) == -7);
    assert(sc::parseOr(string_view("0"), -1) == 0);
    assert(sc::parseOr(string_view(""), 99) == 99);     // empty -> nil
    assert(sc::parseOr(string_view("12x"), 99) == 99);  // trailing junk -> nil
    assert(sc::parseOr(string_view("x12"), 99) == 99);  // leading junk -> nil
    assert(sc::parseOr(string_view(" 12"), 99) == 99);  // leading space -> nil
    assert(sc::parseOr(string_view("+5"), 99) == 99);   // from_chars rejects '+'

    // if-let consumer.
    assert(sc::doubleOr(string_view("21"), -1) == 42);
    assert(sc::doubleOr(string_view("nope"), -1) == -1);

    // render -> parse round-trip.
    assert(sc::roundTrip(0) == 0);
    assert(sc::roundTrip(1234567) == 1234567);
    assert(sc::roundTrip(-99) == -99);

    // slice -> parse over a "key=value" pair.
    assert(sc::valueOf(string_view("width=640"), -1) == 640);
    assert(sc::valueOf(string_view("height=480"), -1) == 480);
    assert(sc::valueOf(string_view("bad=oops"), -1) == -1);  // value not an int
    assert(sc::valueOf(string_view("nodelim"), 7) == 7);     // no '='

    std::puts("string_convert_demo OK");
    return 0;
}
