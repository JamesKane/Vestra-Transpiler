// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#include "cache_tlb.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <print>

int main() {
    using namespace examples::cache_tlb;

    // zero_at writes the address of a stack buffer into the raw
    // mint path. Pre-fill with a non-zero pattern, run zeroData
    // through it, then confirm every byte is now 0.
    std::array<std::uint8_t, 32> buf{};
    buf.fill(0xAB);
    auto addr = reinterpret_cast<std::uint64_t>(buf.data());

    zero_at(addr, static_cast<std::intptr_t>(buf.size()));
    for (std::size_t i = 0; i < buf.size(); ++i) {
        if (buf[i] != 0) {
            std::println(
                "buf[{}] should be 0 after zero_at, got {:#x}", i, static_cast<unsigned>(buf[i]));
            return EXIT_FAILURE;
        }
    }

    // cycle_cache + global_flush are smoke checks — hosted shims
    // are no-ops so the only failure mode is a crash. Run both;
    // success is just reaching the println below.
    cycle_cache(addr, static_cast<std::intptr_t>(buf.size()));
    global_flush();

    std::println("cache_tlb OK");
    return EXIT_SUCCESS;
}
