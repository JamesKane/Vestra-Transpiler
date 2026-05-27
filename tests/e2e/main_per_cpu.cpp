// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#include "per_cpu.hpp"

#include <cstdlib>
#include <print>

int main() {
    using namespace examples::per_cpu;

    // Initial state: both counters zero-init through the @noinit
    // static + PerCpu default ctor + Atomic's constexpr default
    // value-initializer chain.
    if (total_load() != 0) {
        std::println("total_load should start at 0");
        return EXIT_FAILURE;
    }

    // Three requests, one error → fetchAdd returns the prior
    // value. The chain: PerCpu.mine() returns Atomic<T>&; .fetch_add
    // mutates it through the volatile-bus discipline std::atomic
    // provides. (Hosted runs single-threaded; the kernel target
    // gets the per-hart no-false-sharing property for free since
    // each slot is 64-byte aligned.)
    if (record_request() != 0) {
        std::println("first record_request should return prior=0");
        return EXIT_FAILURE;
    }
    if (record_request() != 1) {
        std::println("second record_request should return prior=1");
        return EXIT_FAILURE;
    }
    if (record_request() != 2) {
        std::println("third record_request should return prior=2");
        return EXIT_FAILURE;
    }
    if (record_error() != 0) {
        std::println("first record_error should return prior=0");
        return EXIT_FAILURE;
    }

    // Three requests + one error = 4.
    if (auto t = total_load(); t != 4) {
        std::println("total_load expected 4, got {}", t);
        return EXIT_FAILURE;
    }

    // The PerCpu slots are 64-byte aligned so the kernel target
    // gets the no-false-sharing property for free. Hosted CI can
    // cross-check the alignment statically.
    static_assert(alignof(__vstr::PerCpu<std::atomic<std::uint32_t>>) >= 64);
    static_assert(sizeof(__vstr::PerCpu<std::atomic<std::uint32_t>>) >= 64);

    std::println("per_cpu OK");
    return EXIT_SUCCESS;
}
