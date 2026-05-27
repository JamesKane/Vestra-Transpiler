// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#include "ptr_deref.hpp"

#include <cstdint>
#include <cstdlib>
#include <print>

int main() {
    using namespace examples::ptr_deref;

    // Read: stash a UInt32 on the stack, hand its address to
    // load_word, verify the read returns the same value.
    std::uint32_t cell = 0xDEAD'BEEFu;
    if (auto v = load_word(reinterpret_cast<std::uint64_t>(&cell)); v != cell) {
        std::println("load_word expected {:#x}, got {:#x}", cell, v);
        return EXIT_FAILURE;
    }

    // Write: store a different value through store_word, verify
    // the stack cell holds the new value.
    constexpr std::uint32_t written = 0xCAFE'F00Du;
    store_word(reinterpret_cast<std::uint64_t>(&cell), written);
    if (cell != written) {
        std::println("store_word expected {:#x}, got {:#x}", written, cell);
        return EXIT_FAILURE;
    }

    // RMW: bump by a fixed delta three times. Start from a known
    // value so the totals are deterministic.
    cell = 100;
    if (auto v = bump_word(reinterpret_cast<std::uint64_t>(&cell), 10); v != 110) {
        std::println("bump_word(+10) expected 110, got {}", v);
        return EXIT_FAILURE;
    }
    if (auto v = bump_word(reinterpret_cast<std::uint64_t>(&cell), 20); v != 130) {
        std::println("bump_word(+20) expected 130, got {}", v);
        return EXIT_FAILURE;
    }
    if (auto v = bump_word(reinterpret_cast<std::uint64_t>(&cell), 30); v != 160) {
        std::println("bump_word(+30) expected 160, got {}", v);
        return EXIT_FAILURE;
    }

    std::println("ptr_deref OK");
    return EXIT_SUCCESS;
}
