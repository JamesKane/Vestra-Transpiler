// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#include "boot_chain.hpp"

#include <cstdint>
#include <cstdlib>
#include <print>

int main() {
    using namespace examples::boot_chain;

    // _start is @naked — calling it on a hosted system is undefined
    // (the C++ compiler emitted no prologue / epilogue and the body
    // is empty). The link-time test below just verifies its symbol
    // exists and its address is stable.
    auto* boot_addr = reinterpret_cast<const void*>(&_start);
    if (boot_addr == nullptr) {
        std::println("@boot symbol has no address");
        return EXIT_FAILURE;
    }

    // kernel_init runs once at stage-1. The marker round-trip stands
    // in for any subsystem-init side effect a real kernel would
    // perform between @boot and the first ordinary call.
    if (auto v = kernel_init(7); v != 8) {
        std::println("kernel_init(7) expected 8, got {}", v);
        return EXIT_FAILURE;
    }

    // kernel_main is the ordinary-regime entry the kernel hands off
    // to via the scheduler's first swapContext.
    if (auto v = kernel_main(7); v != 107) {
        std::println("kernel_main(7) expected 107, got {}", v);
        return EXIT_FAILURE;
    }

    // Composing the chain: pretend the marker flows from boot to
    // init to main. @kernel_init bumps by 1, @ordinary bumps by 100.
    if (auto v = kernel_main(kernel_init(0)); v != 101) {
        std::println("chain expected 101, got {}", v);
        return EXIT_FAILURE;
    }

    std::println("boot_chain OK");
    return EXIT_SUCCESS;
}
