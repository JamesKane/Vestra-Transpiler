// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

// E2E driver for the §7 generics-phase-2 first slice (user-defined generic
// structs). Exercises construction with inference, field access through an
// instantiation, and a two-type-parameter struct, then constructs an
// instantiation directly on the C++ side to confirm the emitted template
// matches what the Vestra functions hand back.

#include "generic_struct.hpp"

#include <cassert>
#include <cstdio>

namespace gs = examples::generic_struct;

int main() {
    // Construction inferred from arguments, plus field-access summation.
    assert(gs::sumPair(gs::makePair(3, 4)) == 7);

    // Construction seeded by the annotated binding's type.
    assert(gs::annotatedPair() == 30);

    // Two-type-parameter instantiation, both fields read back.
    auto entry = gs::makeEntry(42, true);
    assert(gs::keyOf(entry) == 42);
    assert(gs::flagOf(entry) == true);

    // The emitted template is an ordinary C++ class template: build a
    // specialization on the C++ side and round-trip it through the Vestra
    // function to prove the two agree on the instantiation.
    gs::Pair<std::int32_t> p{.first = 100, .second = 23};
    assert(gs::sumPair(p) == 123);

    gs::KeyValue<std::int32_t, bool> kv{.key = 7, .value = false};
    assert(gs::keyOf(kv) == 7);
    assert(gs::flagOf(kv) == false);

    std::puts("generic_struct OK");
    return 0;
}
