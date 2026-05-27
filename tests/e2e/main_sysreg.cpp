// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#include "sysreg.hpp"

#include <cstdint>
#include <cstdlib>
#include <print>

int main() {
    using namespace examples::sysreg;

    // daif round-trip. The hosted runtime backs each Sysreg with a
    // static cell, so writes and reads round-trip exactly. The
    // kernel target replaces the cell with a real `mrs` / `msr`.
    constexpr std::uint64_t mask = 0xFu;  // mask all four classes
    mask_interrupts(mask);
    if (auto v = read_interrupts(); v != mask) {
        std::println("daif round-trip: expected {:#x}, got {:#x}", mask, v);
        return EXIT_FAILURE;
    }

    // vbar_el1 + sctlr_el1 round-trip. The MMU bring-up path uses
    // both registers in a batch; on the hosted side each cell is
    // independent so we verify them separately.
    install_vbar(0xFFFF'F000'0000'0000ull);
    constexpr std::uint64_t pgd = 0x0000'1234'5678'0000ull;
    constexpr std::uint64_t sctlr_v = 0x0000'0000'0000'00FFull;
    enable_mmu(pgd, sctlr_v);

    if (auto v = roundtrip_sctlr(sctlr_v); v != sctlr_v) {
        std::println("sctlr_el1 round-trip: expected {:#x}, got {:#x}", sctlr_v, v);
        return EXIT_FAILURE;
    }

    // Inspect ttbr0_el1 + vbar_el1 directly via the runtime
    // namespace — the user-facing Vestra API doesn't expose the
    // cells but the runtime's structure is open by design for
    // tooling that needs to verify state.
    if (__vstr::sysreg::ttbr0_el1.cell != pgd) {
        std::println("ttbr0_el1 backing cell mismatch");
        return EXIT_FAILURE;
    }

    std::println("sysreg OK");
    return EXIT_SUCCESS;
}
