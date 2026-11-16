// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

// E2E driver for §12.4 imported-macro hygiene: `macroapp.use` expands
// `@mlib.addK`, whose template names mlib's `scale` and `k` even where the
// use site has locals of the same names.

#include "main.hpp"

#include <cstdio>

int main() {
    // (1*2 + 10) + (1000*2 + 10) + 7
    const int v = macroapp::use(1);
    std::printf("macro_xmod v=%d\n", v);
    return v == 2029 ? 0 : 1;
}
