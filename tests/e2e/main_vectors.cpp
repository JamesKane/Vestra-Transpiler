// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

// End-to-end harness for examples/vectors.vst.
// Compiled by CTest after `vestra build` has produced vectors.hpp/.cpp.

#include "vectors.hpp"

#include <print>

int main() {
    std::println("compute() = {}", examples::vectors::compute());
    return 0;
}
