// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#include "raw_mint.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <print>

int main() {
    using namespace examples::raw_mint;

    // Reserve a stack-resident buffer; pass its address as a UInt64
    // into the raw-mint primitives. The kernel pattern is identical
    // but with a physical-address frame; here we keep everything in
    // hosted memory so the test is deterministic.
    std::array<std::uint32_t, 8> buf{};
    auto addr = reinterpret_cast<std::uint64_t>(buf.data());

    // touch_first writes 42 into [0] and reads it back.
    if (auto v = touch_first(addr, 8); v != 42) {
        std::println("touch_first expected 42, got {}", v);
        return EXIT_FAILURE;
    }
    // Side-effect check: the underlying buffer now holds 42 too.
    if (buf[0] != 42) {
        std::println("buf[0] should be 42 after touch_first");
        return EXIT_FAILURE;
    }
    // Read-only path: const Ptr + const Span, observing the value
    // the mutable path just wrote.
    if (auto v = read_first(addr, 8); v != 42) {
        std::println("read_first expected 42, got {}", v);
        return EXIT_FAILURE;
    }

    std::println("raw_mint OK");
    return EXIT_SUCCESS;
}
