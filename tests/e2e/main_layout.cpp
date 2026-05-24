// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#include "layout.hpp"

#include <cstddef>
#include <cstdlib>
#include <print>

int main() {
    using namespace examples::layout;

    // @repr(packed): 1 + 4 + 1 = 6 bytes, no padding between fields.
    static_assert(sizeof(WireHeader) == 6, "WireHeader should pack to 6 bytes");
    static_assert(alignof(WireHeader) == 1, "WireHeader should have byte alignment");

    // @repr(align(64)): the struct is over-aligned to a cache line.
    static_assert(alignof(CacheLine) == 64, "CacheLine should be 64-byte aligned");

    // @bits packing: two 4-bit nibbles share a byte.
    static_assert(sizeof(Nibbles) == 1, "Nibbles should occupy one byte");
    Nibbles n{};
    n.lo = 0xA;
    n.hi = 0x5;
    if (n.lo != 0xA || n.hi != 0x5) {
        std::println("nibble round-trip wrong: lo={}, hi={}",
                     static_cast<int>(n.lo),
                     static_cast<int>(n.hi));
        return EXIT_FAILURE;
    }

    std::println("layout OK");
    return EXIT_SUCCESS;
}
