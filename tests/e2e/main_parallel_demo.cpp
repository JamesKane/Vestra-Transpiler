// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

// E2E driver for the §11.2 parallel slice. Runs the worker over disjoint
// chunks of a span and confirms every element is touched exactly once
// (the partition is disjoint and covers the whole span).

#include "parallel_demo.hpp"

#include <array>
#include <cassert>
#include <cstdio>
#include <span>

namespace pd = examples::parallel_demo;

int main() {
    std::array<std::int32_t, 7> a{0, 1, 2, 3, 4, 5, 6};

    // 3 chunks over 7 elements (3, 2, 2): every element incremented once.
    pd::incrementAll(std::span<std::int32_t>(a), 3);
    for (int i = 0; i < 7; ++i) {
        assert(a[i] == i + 1);
    }

    // A different chunk count still covers the whole span exactly once.
    pd::scaleAll(std::span<std::int32_t>(a), 4, 10);
    for (int i = 0; i < 7; ++i) {
        assert(a[i] == (i + 1) * 10);
    }

    // A single chunk and more chunks than elements are both well-formed.
    std::array<std::int32_t, 3> b{5, 5, 5};
    pd::incrementAll(std::span<std::int32_t>(b), 1);
    pd::incrementAll(std::span<std::int32_t>(b), 8);  // more chunks than elements
    for (int i = 0; i < 3; ++i) {
        assert(b[i] == 7);
    }

    std::puts("parallel_demo OK");
    return 0;
}
