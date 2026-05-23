// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

// End-to-end harness for examples/hello.vst.
// Compiled by CTest after `vestra build` has produced hello.hpp/.cpp.

#include "hello.hpp"

#include <print>

int main() {
    std::println("compute() = {}", examples::hello::compute());
    return 0;
}
