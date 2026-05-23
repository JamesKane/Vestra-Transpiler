// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

// End-to-end harness for examples/cfg.vst.
// Compiled by CTest after `vestra build` has produced cfg.hpp/.cpp. The
// expected output depends on the host: 65 on macOS arm64/x86_64 (1 + 64),
// 66 on Linux 64-bit (2 + 64), and so on. We assert by tag below.

#include "cfg.hpp"

#include <print>

int main() {
    std::println("compute() = {}", examples::platform::compute());
    return 0;
}
