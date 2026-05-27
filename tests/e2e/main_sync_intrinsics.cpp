// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#include "sync_intrinsics.hpp"

#include <cstdlib>
#include <print>

int main() {
    using namespace examples::sync_intrinsics;

    // release_with_barrier stores 7 with .release + a storeStore
    // barrier; gate then reads back as 7.
    release_with_barrier(7);
    if (gate.load(std::memory_order_seq_cst) != 7) {
        std::println("gate should be 7 after release_with_barrier");
        return EXIT_FAILURE;
    }
    // reorder_guard is purely a compiler fence — no observable
    // runtime effect, just verify it doesn't crash.
    reorder_guard();
    // hint_chain exercises relax / nop / signalEvent; on the
    // hosted host these are all no-ops or pauses. The return value
    // just reads gate (7 from the prior store).
    if (auto v = hint_chain(); v != 7) {
        std::println("hint_chain expected 7, got {}", v);
        return EXIT_FAILURE;
    }
    // isb_chain stores 42, fires an ISB, then reads back 42.
    if (auto v = isb_chain(42); v != 42) {
        std::println("isb_chain expected 42, got {}", v);
        return EXIT_FAILURE;
    }

    std::println("sync_intrinsics OK");
    return EXIT_SUCCESS;
}
