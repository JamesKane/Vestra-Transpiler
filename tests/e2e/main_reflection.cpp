// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#include "reflection.hpp"

#include <cstdlib>
#include <print>

int main() {
    const auto n0 = examples::reflection::name_at(0);
    const auto n1 = examples::reflection::name_at(1);
    const auto n2 = examples::reflection::name_at(2);
    if (n0 != "x" || n1 != "y" || n2 != "label") {
        std::println("wrong field names: {} {} {}", n0, n1, n2);
        return EXIT_FAILURE;
    }

    const auto t0 = examples::reflection::type_at(0);
    const auto t1 = examples::reflection::type_at(1);
    const auto t2 = examples::reflection::type_at(2);
    if (t0 != "Int32" || t1 != "Int32" || t2 != "Str") {
        std::println("wrong field types: {} {} {}", t0, t1, t2);
        return EXIT_FAILURE;
    }

    std::println("reflection OK: {}:{}, {}:{}, {}:{}", n0, t0, n1, t1, n2, t2);
    return EXIT_SUCCESS;
}
