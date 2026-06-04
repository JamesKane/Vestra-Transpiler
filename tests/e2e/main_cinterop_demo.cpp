// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

// E2E driver for §5 C interop: `import c "clib.h"` emits a global-scope
// `#include "clib.h"`, and an in-tree `@extern` declaration binds to the
// inline `cdemo_value()` that header defines. answer() calls it and returns 42.

#include "cinterop.hpp"

#include <cassert>
#include <cstdio>

int main() {
    assert(answer() == 42);  // calls the C cdemo_value() pulled in via import c
    std::puts("cinterop_demo OK");
    return 0;
}
