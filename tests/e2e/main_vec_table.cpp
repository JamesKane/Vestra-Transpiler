// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#include "vec_table.hpp"

#include <cstdint>
#include <cstdlib>
#include <print>

int main() {
    using namespace examples::vec_table;

    // Dispatch through the vector table by index — what hardware
    // does on a real exception vector entry. Each slot fires its
    // own ISR, which writes a unique sentinel into the frame.
    struct Case {
        std::size_t index;
        std::uint64_t expected_x0;
        std::uint32_t expected_which;
    };
    constexpr Case cases[] = {
        {0, 0xA1A1A1A1ull, 0u},
        {1, 0xB2B2B2B2ull, 1u},
        {2, 0xC3C3C3C3ull, 2u},
        {3, 0xD4D4D4D4ull, 3u},
    };

    for (const auto& c : cases) {
        TrapFrame frame{};
        aarch64VecTable[c.index](frame);
        if (frame.x0 != c.expected_x0) {
            std::println("slot {} expected x0={:#x}, got {:#x}", c.index, c.expected_x0, frame.x0);
            return EXIT_FAILURE;
        }
        if (frame.which != c.expected_which) {
            std::println(
                "slot {} expected which={}, got {}", c.index, c.expected_which, frame.which);
            return EXIT_FAILURE;
        }
    }

    // Verify the table base is what `VBAR_EL1`-style code would
    // write: a pointer to the first slot, which the table-as-
    // pointer decay gives for free. The slot-index pointer
    // arithmetic should match across all 4 entries.
    auto* base = aarch64VecTable.data();
    if (base[0] != &sync_el1 || base[3] != &serror_el1) {
        std::println("vector table base / slot pointers don't match");
        return EXIT_FAILURE;
    }

    std::println("vec_table OK");
    return EXIT_SUCCESS;
}
