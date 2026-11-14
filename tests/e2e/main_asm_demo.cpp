// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

// Driver for examples/asm_demo (§14.4 inline asm, run on an aarch64 host).
#include "asm_demo.hpp"

#include <cassert>
#include <cstdio>

int main() {
    using namespace examples::asm_demo;
    assert(byteSwap(0x0102030405060708ull) == 0x0807060504030201ull);
    assert(addThenDouble(20, 1) == 42);
    assert(viaPinned(35) == 42);
    std::uint64_t n = 41;
    increment(n);
    assert(n == 42);
    assert(splitHalves(0x0000002a00000000ull + 0) == 42);
    assert(percent(42) == 42);
    std::puts("asm_demo OK");
    return 0;
}
