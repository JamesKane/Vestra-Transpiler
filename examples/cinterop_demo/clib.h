// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

// A tiny in-tree C header, pulled into the generated translation unit by
// `import c "clib.h"` (see cinterop.vst). It is a C header, so a C++ includer
// sees C linkage, which is what the Vestra side's `@extern("C")` declaration
// has. The `inline` definition gives the function a single, header-merged
// body.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

inline int cdemo_value() {
    return 42;
}

#ifdef __cplusplus
}
#endif
