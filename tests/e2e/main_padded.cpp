// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#include "padded.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <print>

// The load-bearing property of Padded[T] is sizeof and alignment:
// every cell rounds up to a cache line and starts on a cache-line
// boundary. Validate both at compile time via static_assert — the
// runtime test below would still pass if Padded silently fell back
// to plain T, so the compile-time checks are what guarantee the
// false-sharing-resistance property.
static_assert(alignof(__vstr::Padded<std::uint64_t>) == 64,
              "Padded[UInt64] must align to a cache line");
static_assert(sizeof(__vstr::Padded<std::uint64_t>) == 64,
              "Padded[UInt64] sizeof must round up to a cache line");
static_assert(alignof(__vstr::Padded<std::atomic<std::uint32_t>>) == 64,
              "Padded[Atomic[UInt32]] must align to a cache line");
static_assert(sizeof(__vstr::Padded<std::atomic<std::uint32_t>>) == 64,
              "Padded[Atomic[UInt32]] sizeof must round up to a cache line");

int main() {
    using namespace examples::padded;

    // Padded[UInt64] round-trip via .value.
    constexpr std::uint64_t markers[] = {0, 1, 0xDEAD'BEEF'CAFE'F00Dull};
    for (auto m : markers) {
        set_cell(m);
        if (auto v = get_cell(); v != m) {
            std::println("padded cell expected {:#x}, got {:#x}", m, v);
            return EXIT_FAILURE;
        }
    }

    // Padded[Atomic[UInt32]] — three sequential bumps from zero.
    // The slot is a freshly-noinit static, so initial value is 0;
    // fetchAdd returns the prior value.
    if (auto v = bump_counter(); v != 0) {
        std::println("bump 1: expected prior 0, got {}", v);
        return EXIT_FAILURE;
    }
    if (auto v = bump_counter(); v != 1) {
        std::println("bump 2: expected prior 1, got {}", v);
        return EXIT_FAILURE;
    }
    if (auto v = bump_counter(); v != 2) {
        std::println("bump 3: expected prior 2, got {}", v);
        return EXIT_FAILURE;
    }
    if (auto v = read_counter(); v != 3) {
        std::println("final counter: expected 3, got {}", v);
        return EXIT_FAILURE;
    }

    std::println("padded OK");
    return EXIT_SUCCESS;
}
