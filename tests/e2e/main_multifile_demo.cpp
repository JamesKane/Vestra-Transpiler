// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

// E2E driver for the §5 multi-file demo: `demo.app` (main.vst) imports
// `util.math` (util/math.vst) and calls across the module boundary. Including
// the entry module's header pulls in the dependency's header transitively, so
// both `compute()` (a cross-module call) and `viaConst()` (a cross-module
// const) are reachable and correct.

#include "main.hpp"

#include <cassert>
#include <cstdio>

int main() {
    assert(demo::app::compute() == 42);   // util.math.add(40, 2) across modules
    assert(demo::app::viaConst() == 42);  // util.math.answer across modules
    assert(demo::app::localAdd() == 38);  // the local add (40 - 2) shadows nothing
    assert(demo::app::pairSum() == 42);   // cross-module struct: util.math.Pair
    assert(demo::app::combined() == 42);  // imported fn over the module's own type
    std::puts("multifile_demo OK");
    return 0;
}
