// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

// E2E driver for parse-error reporting (examples/parser_errors_demo.vst). The
// throws(ParseError) parser lowers to a function returning std::expected<Int,
// ParseError>, so the driver inspects both the value path and the *typed error*
// for each kind of malformed input — and evalOr exercises the in-Vestra do/catch
// recovery.

#include "parser_errors_demo.hpp"

#include <cassert>
#include <cstdio>
#include <string_view>

namespace pe = examples::parser_errors_demo;

int main() {
    using std::string_view;

    // Valid input: parse() yields the value.
    auto ok = pe::parse(string_view("2 + 3 * 4"));
    assert(ok.has_value() && ok.value() == 14);
    auto ok2 = pe::parse(string_view("(2 + 3) * 4"));
    assert(ok2.has_value() && ok2.value() == 20);

    // Each kind of malformed input reports its specific typed error.
    auto e1 = pe::parse(string_view("2 + * 3"));  // operator where a factor expected
    assert(!e1.has_value() && e1.error() == pe::ParseError::unexpected);

    auto e2 = pe::parse(string_view("(2 + 3"));  // unclosed group
    assert(!e2.has_value() && e2.error() == pe::ParseError::expectedRParen);

    auto e3 = pe::parse(string_view("2 3"));  // a complete expr, then leftover input
    assert(!e3.has_value() && e3.error() == pe::ParseError::trailing);

    auto e4 = pe::parse(string_view("@"));  // an unknown character
    assert(!e4.has_value() && e4.error() == pe::ParseError::unexpected);

    auto e5 = pe::parse(string_view(""));  // empty input
    assert(!e5.has_value() && e5.error() == pe::ParseError::unexpected);

    // evalOr recovers in Vestra (do/catch): the value, or the fallback on error.
    assert(pe::evalOr(string_view("2 + 3 * 4"), -99) == 14);
    assert(pe::evalOr(string_view("(2 + 3"), -99) == -99);
    assert(pe::evalOr(string_view("2 3"), -99) == -99);

    std::puts("parser_errors_demo OK");
    return 0;
}
