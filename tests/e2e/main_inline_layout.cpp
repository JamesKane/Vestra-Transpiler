// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#include "inline_layout.hpp"

#include <cstdlib>
#include <print>

int main() {
    using namespace examples::inline_layout;

    // CapEntry: UInt64 (8,8) + UInt32 (4,4) + UInt16 (2,2) + UInt8 (1,1)
    // + UInt8 (1,1). Field offsets land at 0, 8, 12, 14, 15 — total
    // running 16, struct-align 8, padded-up to 16. Final: size=16,
    // align=8. Cross-check with the C++ struct's natural layout to
    // catch any drift between Vestra's fold and the host ABI.
    static_assert(sizeof(CapEntry) == 16);
    static_assert(alignof(CapEntry) == 8);
    static_assert(sizeof(Tag) == 8);
    static_assert(alignof(Tag) == 8);
    static_assert(sizeof(TaggedCap) == 24);
    static_assert(alignof(TaggedCap) == 8);
    static_assert(sizeof(Pool) == 16);
    static_assert(alignof(Pool) == 4);

    // layout_total = 16+8 + 8+8 + 24+8 + 16+4 = 92.
    if (auto v = layout_total(); v != 92) {
        std::println("layout_total expected 92, got {}", v);
        return EXIT_FAILURE;
    }
    // inline_total = 4 + (-1) + 0 = 3.
    if (auto v = inline_total(); v != 3) {
        std::println("inline_total expected 3, got {}", v);
        return EXIT_FAILURE;
    }

    std::println("inline_layout OK");
    return EXIT_SUCCESS;
}
