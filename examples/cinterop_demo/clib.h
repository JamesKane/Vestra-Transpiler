// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

// A tiny in-tree C-style header, pulled into the generated translation unit by
// `import c "clib.h"` (see cinterop.vst). The `inline` definition gives the
// function external linkage with a single, header-merged body, so the in-tree
// `@extern` declaration on the Vestra side binds to it at link time.
#pragma once

inline int cdemo_value() {
    return 42;
}
