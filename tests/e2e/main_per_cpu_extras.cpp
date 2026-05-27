// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#include "per_cpu_extras.hpp"

#include <cstdint>
#include <cstdlib>
#include <print>

int main() {
    using namespace examples::per_cpu_extras;

    // bump() drives the local-hart slot through .mine(). Three
    // sequential bumps with deltas {10, 20, 30} should total 60.
    if (auto v = bump(10); v != 10) {
        std::println("bump(10) expected 10, got {}", v);
        return EXIT_FAILURE;
    }
    if (auto v = bump(20); v != 30) {
        std::println("bump(20) expected 30, got {}", v);
        return EXIT_FAILURE;
    }
    if (auto v = bump(30); v != 60) {
        std::println("bump(30) expected 60, got {}", v);
        return EXIT_FAILURE;
    }
    if (auto v = read_slot_via_mine(); v != 60) {
        std::println("read_slot_via_mine() expected 60, got {}", v);
        return EXIT_FAILURE;
    }

    // make_dynamic exercises PerCpu.new on the heap. The seed is
    // the slot's initial value; .mine() reads it back.
    constexpr std::uint64_t cases[] = {0, 1, 0x1234'5678'9ABC'DEF0ull};
    for (auto seed : cases) {
        if (auto v = make_dynamic(seed); v != seed) {
            std::println("make_dynamic({:#x}) expected {:#x}, got {:#x}", seed, seed, v);
            return EXIT_FAILURE;
        }
    }

    std::println("per_cpu_extras OK");
    return EXIT_SUCCESS;
}
