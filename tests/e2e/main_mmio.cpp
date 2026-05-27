// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#include "mmio.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <print>

int main() {
    using namespace examples::mmio;

    // Single-register: write 0xCAFE, read back; the volatile-load
    // sees the volatile-store we just made.
    std::uint32_t reg = 0;
    auto reg_addr = reinterpret_cast<std::uint64_t>(&reg);
    if (auto v = touch_one(reg_addr, 0xCAFE); v != 0xCAFE) {
        std::println("touch_one expected 0xCAFE, got {:#x}", v);
        return EXIT_FAILURE;
    }
    if (reg != 0xCAFE) {
        std::println("reg should be 0xCAFE after touch_one");
        return EXIT_FAILURE;
    }

    // Multi-register: a 4-slot UInt32 region. Sum should be
    // 10 + 20 + 30 + 40 = 100.
    std::array<std::uint32_t, 4> bank{};
    auto bank_addr = reinterpret_cast<std::uint64_t>(bank.data());
    if (auto v = sum_region(bank_addr); v != 100) {
        std::println("sum_region expected 100, got {}", v);
        return EXIT_FAILURE;
    }
    if (bank[0] != 10 || bank[1] != 20 || bank[2] != 30 || bank[3] != 40) {
        std::println("bank should be [10, 20, 30, 40]");
        return EXIT_FAILURE;
    }

    std::println("mmio OK");
    return EXIT_SUCCESS;
}
