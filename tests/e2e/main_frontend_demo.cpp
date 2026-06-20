// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

// E2E driver for the two-phase front-end (examples/frontend_demo.vst): a lexer
// produces a Vec[Token] stream, a recursive-descent parser consumes that stream
// into a Box[Ast] tree, and an evaluator folds it. Checks the lexer's token
// count and the end-to-end value (precedence / associativity / parentheses).

#include "frontend_demo.hpp"

#include <cassert>
#include <cstdio>
#include <string_view>

namespace fe = examples::frontend_demo;

int main() {
    using std::string_view;

    // Phase 1: the lexer's token stream length (operands + operators + `.end`).
    assert(fe::tokenCount(string_view("2 + 3")) == 4);    // num + num end
    assert(fe::tokenCount(string_view("(1+2)*3")) == 8);  // ( 1 + 2 ) * 3 end
    assert(fe::tokenCount(string_view("42")) == 2);       // num end

    // End to end: lex -> parse -> eval, with precedence and parentheses.
    assert(fe::run(string_view("2 + 3 * 4")) == 14);
    assert(fe::run(string_view("(2 + 3) * 4")) == 20);
    assert(fe::run(string_view("10 - 3 - 2")) == 5);  // left-associative
    assert(fe::run(string_view("20 / 2 / 5")) == 2);
    assert(fe::run(string_view("2 * (3 + (4 * 5))")) == 46);  // nesting
    assert(fe::run(string_view("1 + 2 * 3 - 4 / 2 + (5 - 1) * 2")) == 13);
    assert(fe::run(string_view("100")) == 100);

    std::puts("frontend_demo OK");
    return 0;
}
