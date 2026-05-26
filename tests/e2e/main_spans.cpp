// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#include "spans.hpp"

#include <array>
#include <cstdlib>
#include <print>
#include <span>

int main() {
    using namespace examples::spans;

    // The Four const is materialized as a std::array<int32_t, 4>;
    // computeSum() converts it to a std::span<const int32_t> at the
    // sum() call site without any explicit cast at the Vestra layer.
    if (computeSum() != 1 + 2 + 3 + 4) {
        std::println("computeSum wrong: {}", computeSum());
        return EXIT_FAILURE;
    }

    // Direct call with a local std::array — the same call-site coercion.
    std::array<std::int32_t, 5> local{10, 20, 30, 40, 50};
    if (sum(local) != 150) {
        std::println("sum(local) wrong: {}", sum(local));
        return EXIT_FAILURE;
    }

    // Empty span: sumOrZero short-circuits via `.isEmpty`.
    std::array<std::int32_t, 0> empty{};
    if (sumOrZero(empty) != 0) {
        std::println("sumOrZero(empty) wrong");
        return EXIT_FAILURE;
    }

    std::println("spans OK");
    return EXIT_SUCCESS;
}
