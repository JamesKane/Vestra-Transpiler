// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

// E2E driver for the §7 generics-phase-2 const-generic slice. Exercises
// construction with both a type and a const argument inferred, field
// access through the instantiation, and two different lengths, then builds
// an instantiation directly on the C++ side to confirm the emitted
// template matches what the Vestra functions hand back.

#include "const_generic.hpp"

#include <array>
#include <cassert>
#include <cstdio>

namespace cg = examples::const_generic;

int main() {
    // Construction inferring T = Int32 and N = 4, then field access.
    auto b4 = cg::make4();
    assert(cg::firstOf(b4) == 10);
    assert(cg::thirdOf(b4) == 30);

    // A different const argument flows through independently.
    auto b2 = cg::make2();
    assert(cg::sumPairBuf(b2) == 16);

    // The emitted template is an ordinary C++ class template parameterized
    // on a type and a std::size_t: build a specialization directly and
    // round-trip it through the Vestra accessors.
    cg::Buffer<std::int32_t, 4> b{.data = {1, 2, 3, 4}};
    assert(cg::firstOf(b) == 1);
    assert(cg::thirdOf(b) == 3);
    static_assert(sizeof(cg::Buffer<std::int32_t, 4>) == sizeof(std::int32_t) * 4,
                  "const generic N must size the array");

    std::puts("const_generic OK");
    return 0;
}
