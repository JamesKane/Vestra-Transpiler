// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

// End-to-end harness for examples/ownership.vst.
// Compiled by CTest after `vestra build` has produced ownership.hpp/.cpp.

#include "ownership.hpp"

#include <print>

int main() {
    std::println("compute() = {}", examples::ownership::compute());
    return 0;
}
