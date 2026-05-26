// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#include "derive_default.hpp"

#include <cstdlib>
#include <print>

int main() {
    using namespace examples::derive_default;

    // emptyCursor() returns Cursor{} — every field at its zero.
    auto c = emptyCursor();
    if (c.line != 0 || c.column != 0 || c.label.has_value()) {
        std::println("emptyCursor wrong: line={} column={} label_has={}",
                     c.line,
                     c.column,
                     c.label.has_value());
        return EXIT_FAILURE;
    }

    // emptySpan() composes Default through the inner Cursor.
    auto s = emptySpan();
    if (s.start.line != 0 || s.start.column != 0 || s.length != 0 || s.start.label.has_value()) {
        std::println("emptySpan wrong");
        return EXIT_FAILURE;
    }

    // bumpColumn(42) starts from default and writes one field.
    auto b = bumpColumn(42);
    if (b.line != 0 || b.column != 42 || b.label.has_value()) {
        std::println("bumpColumn wrong: line={} column={}", b.line, b.column);
        return EXIT_FAILURE;
    }

    std::println("derive(Default) OK");
    return EXIT_SUCCESS;
}
