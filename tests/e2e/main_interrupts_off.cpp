// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#include "interrupts_off.hpp"

#include <cstdlib>
#include <print>

int main() {
    using namespace examples::interrupts_off;

    // critical_bump increments counter by 1 inside InterruptsOff.
    // Hosted CI runs this as plain code (no real mask manipulation);
    // we just check the counter advanced.
    critical_bump();
    if (counter.load(std::memory_order_seq_cst) != 1) {
        std::println("counter should be 1 after critical_bump");
        return EXIT_FAILURE;
    }

    // nested_bump: counter was 1; inner stores 1+10=11, outer
    // reloads (11) and stores 11+1=12. End state: 12.
    nested_bump();
    if (counter.load(std::memory_order_seq_cst) != 12) {
        std::println("counter should be 12 after nested_bump, got {}",
                     counter.load(std::memory_order_seq_cst));
        return EXIT_FAILURE;
    }

    // Scheduler.swapContext is a no-op on hosted — the call must
    // compile and execute without crashing. The kernel target
    // would actually swap PC/SP/callee-saved registers here.
    switch_tasks();

    // Confirm both Context slots are at least the size we declared.
    static_assert(sizeof(__vstr::Context) >= 256);
    static_assert(alignof(__vstr::Context) >= 16);

    std::println("interrupts_off OK");
    return EXIT_SUCCESS;
}
