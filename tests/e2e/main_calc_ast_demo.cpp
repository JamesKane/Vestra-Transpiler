// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

// E2E driver for the materialized-AST calculator (examples/calc_ast_demo.vst).
// `evalStr` parses to a recursive Box[Ast] tree and folds it to a value;
// `renderStr` walks the same tree to a fully-parenthesized string. The render
// checks pin the *tree shape* (precedence/associativity made structural), not
// just the final number.

#include "calc_ast_demo.hpp"

#include <cassert>
#include <cstdio>
#include <string_view>

namespace ca = examples::calc_ast_demo;

int main() {
    using std::string_view;

    // Evaluation over the built tree — precedence, associativity, parens, nesting.
    assert(ca::evalStr(string_view("42")) == 42);
    assert(ca::evalStr(string_view("2 + 3 * 4")) == 14);
    assert(ca::evalStr(string_view("(2 + 3) * 4")) == 20);
    assert(ca::evalStr(string_view("10 - 3 - 2")) == 5);
    assert(ca::evalStr(string_view("2 * (3 + (4 * 5))")) == 46);
    assert(ca::evalStr(string_view("1 + 2 * 3 - 4 / 2 + (5 - 1) * 2")) == 13);

    // Rendering the tree fully parenthesized makes the structure visible: the
    // parser must have grouped * under + (precedence) and folded - left.
    assert(ca::renderStr(string_view("2 + 3 * 4")) == "(2 + (3 * 4))");
    assert(ca::renderStr(string_view("2 * 3 + 4")) == "((2 * 3) + 4)");
    assert(ca::renderStr(string_view("10 - 3 - 2")) == "((10 - 3) - 2)");
    assert(ca::renderStr(string_view("(2 + 3) * 4")) == "((2 + 3) * 4)");
    assert(ca::renderStr(string_view("1 + 2 + 3 + 4")) == "(((1 + 2) + 3) + 4)");
    assert(ca::renderStr(string_view("42")) == "42");

    std::puts("calc_ast_demo OK");
    return 0;
}
