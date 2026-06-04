// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

// E2E driver for §13 Soa[T] (struct-of-arrays). sumX builds a Soa[Point] and
// folds the x column; yAt gathers a full row. Both exercise new/push/len/get
// over the tuple-of-vectors lowering.

#include "soa_demo.hpp"

#include <cassert>
#include <cstdio>

namespace sd = examples::soa_demo;

int main() {
    assert(sd::sumX(4) == 0 + 1 + 2 + 3);  // x column: 0..3 -> 6
    assert(sd::yAt(4, 2) == 102);          // row 2 gathered: y = x + 100
    assert(sd::sumX(0) == 0);              // empty Soa
    std::puts("soa_demo OK");
    return 0;
}
