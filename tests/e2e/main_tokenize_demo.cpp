// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

// E2E driver for §18.5 byte/char access for hand-tokenizing. Exercises charAt
// (non-allocating indexed Char read), chars() iteration, the ASCII classifiers
// (isDigit / isAlpha / isAlnum / isSpace), Char-vs-Char equality, and two
// hand-written scanning tokenizers (identifier and number runs).

#include "tokenize_demo.hpp"

#include <cassert>
#include <cstdio>
#include <string_view>

namespace tk = examples::tokenize_demo;

int main() {
    using std::string_view;

    // charAt: first char, nil when empty.
    assert(tk::firstChar(string_view("hello")).value() == U'h');
    assert(!tk::firstChar(string_view("")).has_value());

    // countChar: charAt loop + Char-vs-Char equality.
    assert(tk::countChar(string_view("a+b+c"), U'+') == 2);
    assert(tk::countChar(string_view("mississippi"), U's') == 4);
    assert(tk::countChar(string_view("abc"), U'z') == 0);

    // chars().len() — total character count.
    assert(tk::charCount(string_view("hello")) == 5);
    assert(tk::charCount(string_view("")) == 0);

    // Classification over chars().
    assert(tk::digitCount(string_view("a1b22c333")) == 6);
    assert(tk::digitCount(string_view("no digits")) == 0);
    assert(tk::spaceCount(string_view("a b\tc\nd")) == 3);
    assert(tk::spaceCount(string_view("tight")) == 0);

    // Hand-written scanning tokenizers over a real expression.
    //   "foo + bar123 * 42 - x9" : identifiers foo, bar123, x9 (3);
    //                              number runs 42 (1).
    assert(tk::identifierCount(string_view("foo + bar123 * 42 - x9")) == 3);
    assert(tk::numberCount(string_view("foo + bar123 * 42 - x9")) == 1);
    // A digit run inside an identifier (bar123) is part of the identifier, not a
    // separate number — the tokenizer's maximal-munch lookahead handles that.
    assert(tk::numberCount(string_view("bar123")) == 0);
    assert(tk::identifierCount(string_view("bar123")) == 1);
    // Pure numbers and operators.
    assert(tk::numberCount(string_view("10 200 3000")) == 3);
    assert(tk::identifierCount(string_view("10 200 3000")) == 0);
    assert(tk::identifierCount(string_view("+-*/")) == 0);
    assert(tk::numberCount(string_view("+-*/")) == 0);
    // Empty input.
    assert(tk::identifierCount(string_view("")) == 0);
    assert(tk::numberCount(string_view("")) == 0);

    std::puts("tokenize_demo OK");
    return 0;
}
