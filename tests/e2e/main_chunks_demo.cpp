// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

// E2E driver for the §5/§18.4 chunks(of:) partition primitive: a chunked
// sum (chunking is sum-preserving), the oversized-chunk and zero-chunk edge
// cases, and an in-place double of every element via mutable chunks.

#include "chunks_demo.hpp"

#include <array>
#include <cassert>
#include <cstdio>

namespace cd = examples::chunks_demo;

int main() {
    std::array<std::int32_t, 5> a{1, 2, 3, 4, 5};

    // Chunking is sum-preserving: [1,2] + [3,4] + [5] = 15.
    assert(cd::sumChunks(std::span<const std::int32_t>(a), 2) == 15);
    // A chunk larger than the span yields one chunk: still 15.
    assert(cd::sumChunks(std::span<const std::int32_t>(a), 99) == 15);
    // A chunk size of 0 yields no chunks: sum 0.
    assert(cd::sumChunks(std::span<const std::int32_t>(a), 0) == 0);

    // first-class: chunks of 2 are [1,2] [3,4] [5]; even-indexed (0, 2) are
    // [1,2] and [5] -> 1+2+5 = 8, via random access (.count + [])
    assert(cd::sumEvenChunks(std::span<const std::int32_t>(a), 2) == 8);

    // Mutate every element through the disjoint mutable chunks.
    cd::doubleChunks(std::span<std::int32_t>(a), 2);
    assert(a[0] == 2 && a[1] == 4 && a[2] == 6 && a[3] == 8 && a[4] == 10);

    std::puts("chunks_demo OK");
    return 0;
}
