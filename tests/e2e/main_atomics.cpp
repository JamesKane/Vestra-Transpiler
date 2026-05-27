// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#include "atomics.hpp"

#include <cstdlib>
#include <print>

int main() {
    using namespace examples::atomics;

    // bump() resets the counter to 0, fetchAdds 7 (returning 0),
    // reads 7, sums to 7.
    if (auto v = bump(); v != 7) {
        std::println("bump expected 7, got {}", v);
        return EXIT_FAILURE;
    }
    // drain() fetchSubs 7 from the post-bump 7, returning 7.
    if (auto v = drain(); v != 7) {
        std::println("drain expected 7, got {}", v);
        return EXIT_FAILURE;
    }
    // counter is now 0; swap(99) sets it to 99 and returns 0.
    if (auto v = swap(99); v != 0) {
        std::println("swap expected 0, got {}", v);
        return EXIT_FAILURE;
    }
    if (counter.load(std::memory_order_seq_cst) != 99) {
        std::println("counter should be 99 after swap, got {}",
                     counter.load(std::memory_order_seq_cst));
        return EXIT_FAILURE;
    }
    // raise() flips flag from false → true; lowered() returns
    // (flag == false), i.e., false after raise.
    raise();
    if (lowered()) {
        std::println("flag should be raised");
        return EXIT_FAILURE;
    }

    // §A4 (§14.9.3) compareExchange success: cas_target starts at 0,
    // CAS(0, 42) succeeds, .actual = 0 (prior value).
    if (auto v = cas_success(42); v != 0) {
        std::println("cas_success expected 0, got {}", v);
        return EXIT_FAILURE;
    }
    if (cas_target.load(std::memory_order_seq_cst) != 42) {
        std::println("cas_target should now be 42");
        return EXIT_FAILURE;
    }
    // CAS(99, 7) with current value 42 → fails. .actual = 42.
    if (auto v = cas_failure(99, 7); v != 42) {
        std::println("cas_failure expected actual=42, got {}", v);
        return EXIT_FAILURE;
    }
    if (cas_target.load(std::memory_order_seq_cst) != 42) {
        std::println("cas_target should still be 42 after failed CAS");
        return EXIT_FAILURE;
    }

    // §A4 (§14.9.2) bitwise chain. 0xF0 + 0xFF + 0xFE + 0xFF = 0x3EC.
    if (auto v = bitwise_chain(); v != 0x3EC) {
        std::println("bitwise_chain expected 0x3EC, got {:#x}", v);
        return EXIT_FAILURE;
    }

    std::println("atomics OK");
    return EXIT_SUCCESS;
}
