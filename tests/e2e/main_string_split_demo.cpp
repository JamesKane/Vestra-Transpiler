// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

// E2E driver for §18.5 String/Str.split(sep). Exercises the materialized
// Vec[Str] of pieces through the existing Vec surface (for-in, len, get),
// the empty-piece edge cases (adjacent / leading / trailing separators), an
// empty separator, and the split+toInt / split+query combinations.

#include "string_split_demo.hpp"

#include <cassert>
#include <cstdio>
#include <string_view>

namespace ss = examples::string_split_demo;

int main() {
    using std::string_view;

    // Piece counts: occurrences + 1, including empty pieces.
    assert(ss::pieceCount(string_view("a,b,c"), string_view(",")) == 3);
    assert(ss::pieceCount(string_view("a"), string_view(",")) == 1);     // no sep -> 1
    assert(ss::pieceCount(string_view(""), string_view(",")) == 1);      // empty -> [""]
    assert(ss::pieceCount(string_view("a,,b"), string_view(",")) == 3);  // adjacent -> empty piece
    assert(ss::pieceCount(string_view(",a,"), string_view(",")) == 3);   // leading + trailing
    assert(ss::pieceCount(string_view("a,b"), string_view("")) == 1);  // empty sep -> whole string
    // Multi-char separator.
    assert(ss::pieceCount(string_view("a::b::c"), string_view("::")) == 3);

    // Indexed field access with an out-of-range default.
    assert(ss::field(string_view("x,y,z"), string_view(","), 0) == "x");
    assert(ss::field(string_view("x,y,z"), string_view(","), 2) == "z");
    assert(ss::field(string_view("x,y,z"), string_view(","), 9).empty());  // out of range
    assert(ss::field(string_view("a,,b"), string_view(","), 1).empty());   // the empty middle piece

    // split + toInt: sum the integer fields, skipping non-integers.
    assert(ss::sumCsv(string_view("1,2,3")) == 6);
    assert(ss::sumCsv(string_view("10,x,20")) == 30);  // "x" skipped
    assert(ss::sumCsv(string_view("")) == 0);          // one empty piece, not an int
    assert(ss::sumCsv(string_view("5")) == 5);
    assert(ss::sumCsv(string_view("-3,3")) == 0);

    // Word counting via the non-empty pieces.
    assert(ss::wordCount(string_view("the quick brown fox")) == 4);
    assert(ss::wordCount(string_view("a  b")) == 2);  // double space -> empty piece skipped
    assert(ss::wordCount(string_view("")) == 0);
    assert(ss::wordCount(string_view("solo")) == 1);

    // split feeding the other queries.
    assert(ss::valueLen(string_view("width=640")) == 3);
    assert(ss::valueLen(string_view("k=")) == 0);       // empty value
    assert(ss::valueLen(string_view("nodelim")) == 0);  // no '='

    std::puts("string_split_demo OK");
    return 0;
}
