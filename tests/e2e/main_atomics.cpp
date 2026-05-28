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

    // §A4 (§14.9.3) compareExchangeWeak inside a while-true retry
    // loop. Single-threaded, so the weak CAS converges within one
    // or two spurious-fail iterations on every host.
    if (auto v = retry_loop_set(0, 17); v != 0) {
        std::println("retry_loop_set expected actual=0, got {}", v);
        return EXIT_FAILURE;
    }
    if (weak_target.load(std::memory_order_seq_cst) != 17) {
        std::println("weak_target should be 17 after retry_loop_set");
        return EXIT_FAILURE;
    }
    // Shape 1: while !r.succeeded. Bumps weak_target from 17 → 42.
    if (auto v = retry_loop_until(17, 42); v != 17) {
        std::println("retry_loop_until expected actual=17, got {}", v);
        return EXIT_FAILURE;
    }
    if (weak_target.load(std::memory_order_seq_cst) != 42) {
        std::println("weak_target should be 42 after retry_loop_until");
        return EXIT_FAILURE;
    }

    // §A4 (§14.9.4) wide-atomic surface. Cross-check via direct
    // reads against std::atomic<__uint128_t> / std::atomic<__int128_t>.
    // __uint128_t doesn't have a std::print formatter, so cast the
    // low 64 bits when reporting mismatches.
    {
        __uint128_t delta = wide_widen(0x100);  // explicit widening
        if (auto v = wide_bump(delta); v != delta) {
            std::println("wide_bump expected delta low={:#x}, got low={:#x}",
                         static_cast<std::uint64_t>(delta),
                         static_cast<std::uint64_t>(v));
            return EXIT_FAILURE;
        }
        if (auto v = wide_swap(0); v != delta) {
            std::println("wide_swap expected prior={:#x} low, got low={:#x}",
                         static_cast<std::uint64_t>(delta),
                         static_cast<std::uint64_t>(v));
            return EXIT_FAILURE;
        }
        if (wide_counter.load(std::memory_order_seq_cst) != 0) {
            std::println("wide_counter should be 0 after swap");
            return EXIT_FAILURE;
        }
    }
    {
        // wide_cas starts at 0; success path mirrors the 32-bit case.
        if (auto v = wide_cas_success(0x42); v != 0) {
            std::println("wide_cas_success expected 0, got low={:#x}",
                         static_cast<std::uint64_t>(v));
            return EXIT_FAILURE;
        }
        if (wide_cas.load(std::memory_order_seq_cst) != 0x42U) {
            std::println("wide_cas should be 0x42 after success path");
            return EXIT_FAILURE;
        }
        if (auto v = wide_cas_failure(0x99, 0x7); v != 0x42U) {
            std::println("wide_cas_failure expected actual=0x42, got low={:#x}",
                         static_cast<std::uint64_t>(v));
            return EXIT_FAILURE;
        }
    }
    {
        // Signed wide: fetchAdd of +5 returns the pre-op 0.
        if (auto v = signed_wide_bump(static_cast<__int128_t>(5)); v != 0) {
            std::println("signed_wide_bump expected prior=0, got low={:#x}",
                         static_cast<std::uint64_t>(v));
            return EXIT_FAILURE;
        }
        if (signed_wide.load(std::memory_order_seq_cst) != 5) {
            std::println("signed_wide should be 5 after fetchAdd");
            return EXIT_FAILURE;
        }
    }

    // §A4 (§14.9.5) AtomicTaggedPointer surface. Single-threaded
    // exercises pin the call shape; the tag auto-bumps by one on
    // every successful CAS, the failure path reports the observed
    // (ptr, tag) via .actual.
    {
        StackNode n1{};
        n1.next = 0;
        StackNode n2{};
        n2.next = 0;
        if (atp_initial_tag() != 0) {
            std::println("atp_initial_tag should be 0");
            return EXIT_FAILURE;
        }
        if (auto t = atp_push(&n1); t != 1) {
            std::println("atp_push first call: tag should be 1, got {}", t);
            return EXIT_FAILURE;
        }
        auto [cur1, cur1_tag] = stack_head.load(std::memory_order_acquire);
        if (cur1 != &n1 || cur1_tag != 1) {
            std::println("stack_head should be (&n1, 1) after first push");
            return EXIT_FAILURE;
        }
        if (auto t = atp_push(&n2); t != 2) {
            std::println("atp_push second call: tag should be 2, got {}", t);
            return EXIT_FAILURE;
        }
        // Failure path: wrong tag, CAS observes the current (2) and
        // returns it via .actual.
        if (auto t = atp_cas_failure(&n2, 999, &n1); t != 2) {
            std::println("atp_cas_failure should report observed tag 2, got {}", t);
            return EXIT_FAILURE;
        }
    }

    std::println("atomics OK");
    return EXIT_SUCCESS;
}
