// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

// E2E driver for §18.5 Vec[T]: a growable sequence lowering to std::vector<T>.
// Exercises push / len / index over both a primitive and a struct element, plus
// the optional-returning get(i) -> T? and pop() -> T?.

#include "vec_demo.hpp"

#include <cassert>
#include <cstdio>

namespace vd = examples::vec_demo;

int main() {
    assert(vd::sumTo(5) == 15);          // 1+2+3+4+5, built by push, read by index
    assert(vd::sumTo(0) == 0);           // empty Vec
    assert(vd::centroidX(4) == 6);       // x = 0+1+2+3 over a Vec[Point]
    assert(vd::getOr(5, 2, -1) == 3);    // in range: xs == [1..5], xs.get(2) == 3
    assert(vd::getOr(5, 99, -1) == -1);  // out of range -> default via ??
    assert(vd::getOr(0, 0, -1) == -1);   // empty Vec -> default
    assert(vd::drainSum(5) == 15);       // pop drains 5+4+3+2+1
    assert(vd::drainSum(0) == 0);        // empty Vec -> pop is nil immediately
    std::puts("vec_demo OK");
    return 0;
}
