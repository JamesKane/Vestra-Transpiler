// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

// E2E driver for the §7 generics-phase-2 generic-enum slice. Exercises
// payloaded + no-payload case construction with inference, match over a
// generic instantiation (payload binding at the argument type), and a
// two-type-parameter enum, then builds an instantiation directly on the
// C++ side to confirm the emitted template matches what Vestra hands back.

#include "generic_enum.hpp"

#include <cassert>
#include <cstdio>
#include <variant>

namespace ge = examples::generic_enum;

int main() {
    // Payloaded construction (T inferred from the argument) + match.
    assert(ge::unwrapOr(ge::someInt(7), -1) == 7);
    // No-payload construction (resolved against the return type) + match.
    assert(ge::unwrapOr(ge::noneInt(), -1) == -1);

    // Two-type-parameter enum, both arms.
    assert(ge::leftOr(ge::makeLeft(42), 99) == 42);
    assert(ge::leftOr(ge::makeRight(true), 99) == 99);  // right(true)  -> fallback
    assert(ge::leftOr(ge::makeRight(false), 99) == 0);  // right(false) -> 0

    // The emitted template is an ordinary C++ class template: build a
    // specialization directly and round-trip it through the Vestra match.
    ge::Maybe<std::int32_t> j{.value = ge::Maybe<std::int32_t>::just_t{123}};
    assert(ge::unwrapOr(j, -1) == 123);

    ge::Maybe<std::int32_t> n{.value = ge::Maybe<std::int32_t>::nothing_t{}};
    assert(ge::unwrapOr(n, 55) == 55);

    // The variant index reflects the active alternative.
    assert(j.value.index() == 0);
    assert(n.value.index() == 1);

    std::puts("generic_enum OK");
    return 0;
}
