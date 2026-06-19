// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

// E2E driver for examples/movelist_demo.vst — a move-only recursive list built
// via a tuple-returning builder and folded with mixed-arm matches. Exercises the
// move-on-consume pass and the match-result-type fix together end to end.

#include "movelist_demo.hpp"

#include <cassert>
#include <cstdio>

namespace ml = examples::movelist_demo;

int main() {
    // [1, 5) = {1, 2, 3, 4}.
    assert(ml::sumRange(1, 5) == 10);
    assert(ml::lenRange(1, 5) == 4);
    assert(ml::maxRange(1, 5) == 4);

    // Single element [7, 8) = {7}.
    assert(ml::sumRange(7, 8) == 7);
    assert(ml::lenRange(7, 8) == 1);
    assert(ml::maxRange(7, 8) == 7);

    // Empty range — the bare-literal `case .empty: 0` base case.
    assert(ml::sumRange(3, 3) == 0);
    assert(ml::lenRange(3, 3) == 0);
    assert(ml::maxRange(3, 3) == 0);

    // A longer list.
    assert(ml::sumRange(1, 11) == 55);  // 1..10
    assert(ml::lenRange(1, 11) == 10);
    assert(ml::maxRange(1, 11) == 10);

    std::puts("movelist_demo OK");
    return 0;
}
