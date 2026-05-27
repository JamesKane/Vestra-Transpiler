// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#include "mmio_union.hpp"

#include <cstdint>
#include <cstdlib>
#include <print>

int main() {
    using namespace examples::mmio_union;

    // Probe the bit ordering with a few crafted words. Under the
    // GCC/Clang packed bit-field layout we use, `rxReady` sits at
    // bit 0, `rxOverrun` at bit 1, `txEmpty` at bit 2 on little-
    // endian hosts. The aarch64 CI host is little-endian.
    struct Case {
        std::uint32_t raw;
        bool rx;
        bool ovr;
        bool tx;
    };
    constexpr Case cases[] = {
        {0u, false, false, false},
        {0b001u, true, false, false},
        {0b010u, false, true, false},
        {0b100u, false, false, true},
        {0b101u, true, false, true},
        {0b111u, true, true, true},
        {0xFFFF'FFFFu, true, true, true},
    };

    for (const auto& c : cases) {
        if (is_rx_ready(c.raw) != c.rx) {
            std::println("is_rx_ready({:#x}) expected {}, got {}", c.raw, c.rx, is_rx_ready(c.raw));
            return EXIT_FAILURE;
        }
        if (is_rx_overrun(c.raw) != c.ovr) {
            std::println(
                "is_rx_overrun({:#x}) expected {}, got {}", c.raw, c.ovr, is_rx_overrun(c.raw));
            return EXIT_FAILURE;
        }
        if (is_tx_empty(c.raw) != c.tx) {
            std::println("is_tx_empty({:#x}) expected {}, got {}", c.raw, c.tx, is_tx_empty(c.raw));
            return EXIT_FAILURE;
        }
    }

    std::println("mmio_union OK");
    return EXIT_SUCCESS;
}
