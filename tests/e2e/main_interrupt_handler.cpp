// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#include "interrupt_handler.hpp"

#include <cstdlib>
#include <print>

int main() {
    using namespace examples::interrupt_handler;

    // Build a trap frame as if the CPU just took a sync exception
    // from EL1. The handler reads/writes through inout, so we pass
    // a stack-resident frame and verify the mutations after return.
    TrapFrame frame{
        .x0 = 41,        // syscall arg
        .x1 = 0,         // unused
        .elr = 0x1000,   // PC at exception
        .esr = 0x12345,  // pretend syscall class
    };

    handle_sync_el1(frame);
    if (frame.x0 != 42) {
        std::println("handle_sync_el1: x0 should be 42, got {}", frame.x0);
        return EXIT_FAILURE;
    }
    if (frame.elr != 0x1004) {
        std::println("handle_sync_el1: elr should be 0x1004, got {:#x}", frame.elr);
        return EXIT_FAILURE;
    }

    // Reset and run the IRQ path.
    frame.x0 = 0xDEAD;
    frame.x1 = 0;
    frame.esr = 30;  // pretend IRQ number
    handle_irq_el1(frame);
    if (frame.x0 != 0) {
        std::println("handle_irq_el1: x0 should be 0 (handled), got {:#x}", frame.x0);
        return EXIT_FAILURE;
    }
    if (frame.x1 != 30) {
        std::println("handle_irq_el1: x1 should be 30 (irq#), got {}", frame.x1);
        return EXIT_FAILURE;
    }

    std::println("interrupt_handler OK");
    return EXIT_SUCCESS;
}
