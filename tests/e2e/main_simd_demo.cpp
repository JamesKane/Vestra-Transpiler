// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

// E2E driver for §13 elementwise vector arithmetic on [N]T. axpy and dot4
// exercise +, *, and lane indexing over std::array-backed fixed vectors.

#include "simd_demo.hpp"

#include <array>
#include <cassert>
#include <cstdio>

namespace sd = examples::simd_demo;

int main() {
    std::array<std::int32_t, 4> a{2, 2, 2, 2};
    std::array<std::int32_t, 4> x{1, 2, 3, 4};
    std::array<std::int32_t, 4> y{10, 20, 30, 40};
    auto r = sd::axpy(a, x, y);  // a*x + y = [12, 24, 36, 48]
    assert(r[0] == 12 && r[1] == 24 && r[2] == 36 && r[3] == 48);
    assert(sd::dot4(x, x) == 1 + 4 + 9 + 16);  // sum of squares = 30
    std::puts("simd_demo OK");
    return 0;
}
