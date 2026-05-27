// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#include "no_auto_barrier.hpp"

#include <cstdint>
#include <cstdlib>
#include <print>

int main() {
    using namespace examples::no_auto_barrier;

    // @no_auto_barrier has no v0.5 codegen effect — the Sysreg
    // auto-barrier insertion isn't shipped yet, so the attribute
    // is a contract marker. The runtime verifies the function
    // still behaves as written, regardless of the marker.
    constexpr std::uint64_t ttbr0 = 0x0000'1234'5678'0000ull;
    constexpr std::uint64_t sctlr = 0x0000'0000'0000'00FFull;
    if (auto v = enable_mmu_aarch64(ttbr0, sctlr); v != (ttbr0 ^ sctlr)) {
        std::println("enable_mmu_aarch64 expected {:#x}, got {:#x}", ttbr0 ^ sctlr, v);
        return EXIT_FAILURE;
    }

    if (auto v = install_vector_table(0xC0); v != 0xC1) {
        std::println("install_vector_table expected 0xC1, got {:#x}", v);
        return EXIT_FAILURE;
    }

    std::println("no_auto_barrier OK");
    return EXIT_SUCCESS;
}
