// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

// End-to-end harness for examples/shapes.vst.
// Compiled by CTest after `vestra build` has produced shapes.hpp/.cpp.

#include "shapes.hpp"

#include <print>

int main() {
    std::println("compute() = {}", examples::shapes::compute());
    return 0;
}
