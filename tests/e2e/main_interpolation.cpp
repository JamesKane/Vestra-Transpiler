// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#include "interpolation.hpp"

#include <cstdlib>
#include <print>
#include <string>

int main() {
    const std::string g = examples::interpolation::greet("Ada", 41);
    if (g != "hello Ada, age 42") {
        std::println("greet wrong: {}", g);
        return EXIT_FAILURE;
    }

    const std::string d = examples::interpolation::describe("disk", 7);
    if (d != "kind={disk}, n=14") {
        std::println("describe wrong: {}", d);
        return EXIT_FAILURE;
    }

    std::println("interpolation OK");
    return EXIT_SUCCESS;
}
