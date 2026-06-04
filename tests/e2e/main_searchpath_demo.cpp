// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

// E2E driver for §5 import search roots: the entry (app/main.vst) imports a
// module (mathx) that lives only under a separate `-I` root (lib/). The build
// resolves it via the search path, emits it at its import path (mathx.{hpp,
// cpp}), and the cross-module call links and runs.

#include "spmain.hpp"

#include <cassert>
#include <cstdio>

int main() {
    assert(app::compute() == 42);  // mathx.triple(14), mathx found via -I
    std::puts("searchpath_demo OK");
    return 0;
}
