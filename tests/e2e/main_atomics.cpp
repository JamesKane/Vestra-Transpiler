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

    std::println("atomics OK");
    return EXIT_SUCCESS;
}
