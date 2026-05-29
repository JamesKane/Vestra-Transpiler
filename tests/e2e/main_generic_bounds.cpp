// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

// E2E driver for the §7 generics-phase-2 protocol-bounds slice. Confirms
// that bounded generic functions instantiate and run for both a primitive
// and a user struct that derives the required protocol, and that the
// emitted `requires` clauses are satisfied (the program would not compile
// otherwise).

#include "generic_bounds.hpp"

#include <cassert>
#include <cstdio>

namespace gb = examples::generic_bounds;

int main() {
    // Eq bound satisfied by a primitive.
    assert(gb::intsEqual(3, 3) == true);
    assert(gb::intsEqual(3, 4) == false);

    // Comparable bound satisfied by a primitive.
    assert(gb::biggest(7, 2) == 7);
    assert(gb::biggest(2, 9) == 9);

    // Eq bound satisfied by a struct via derive(Eq).
    gb::Point p{.x = 1, .y = 2};
    gb::Point q{.x = 1, .y = 2};
    gb::Point r{.x = 3, .y = 4};
    assert(gb::pointsEqual(p, q) == true);
    assert(gb::pointsEqual(p, r) == false);

    // The bounded template is callable directly from C++ too; the requires
    // clause is satisfied by Int32 / Point.
    assert(gb::areEqual<std::int32_t>(5, 5) == true);

    std::puts("generic_bounds OK");
    return 0;
}
