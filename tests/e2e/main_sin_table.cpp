// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#include "sin_table.hpp"

#include <cmath>
#include <cstdlib>
#include <print>

int main() {
    const double sum = examples::sin_table::cycle_sum();
    // A complete sine cycle integrates to zero. The discrete sum here
    // accumulates 32 rounded doubles; the residual is bounded by a
    // small multiple of the machine epsilon (~2.2e-16 per term, so
    // 32*2.2e-16 ≈ 7e-15 is a generous ceiling).
    constexpr double Tolerance = 1e-12;
    if (std::abs(sum) > Tolerance) {
        std::println("cycle_sum() = {} (out of tolerance)", sum);
        return EXIT_FAILURE;
    }

    // Spot-check the table at the cardinal angles: sin(0)=0, sin(π/2)=1,
    // sin(π)=0, sin(3π/2)=-1. Indices are 0, 8, 16, 24 for a 32-sample
    // single-cycle table.
    const double s0 = examples::sin_table::sample_at(0);
    const double s8 = examples::sin_table::sample_at(8);
    const double s16 = examples::sin_table::sample_at(16);
    const double s24 = examples::sin_table::sample_at(24);
    if (std::abs(s0) > Tolerance || std::abs(s8 - 1.0) > Tolerance || std::abs(s16) > Tolerance
        || std::abs(s24 + 1.0) > Tolerance) {
        std::println("cardinal samples wrong: {} {} {} {}", s0, s8, s16, s24);
        return EXIT_FAILURE;
    }

    std::println("sin_table OK: |cycle_sum|={:e}", std::abs(sum));
    return EXIT_SUCCESS;
}
