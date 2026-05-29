// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

// E2E driver for the §5/§18.4 split(at:) partition primitive: a read-only
// split-and-sum, an in-place mutate of the two disjoint halves, and the
// clamped out-of-range split.

#include "split_demo.hpp"

#include <array>
#include <cassert>
#include <cstdio>

namespace sd = examples::split_demo;

int main() {
    std::array<std::int32_t, 5> a{1, 2, 3, 4, 5};

    // [1,2] + [3,4,5] = 15; both halves summed.
    assert(sd::sumSpan(std::span<std::int32_t>(a), 2) == 15);
    // Out-of-range split is clamped: all prefix, empty suffix.
    assert(sd::sumSpan(std::span<std::int32_t>(a), 99) == 15);
    assert(sd::sumSpan(std::span<std::int32_t>(a), 0) == 15);

    // Mutate the two disjoint halves independently.
    sd::transform(std::span<std::int32_t>(a), 2);  // negate [1,2], double [3,4,5]
    assert(a[0] == -1 && a[1] == -2);
    assert(a[2] == 6 && a[3] == 8 && a[4] == 10);

    std::puts("split_demo OK");
    return 0;
}
