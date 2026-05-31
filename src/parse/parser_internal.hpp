// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#pragma once

// Internal helpers shared across the parser translation units (parser.cpp
// and its parser_*.cpp siblings). `merge` is used by nearly every parse
// production to span two source ranges; hoisting it here as `inline` lets
// the file be split by theme without duplicating it. Not part of the public
// parse API — do not include outside src/parse/.

#include "vestra/diag/source_manager.hpp"

namespace vestra::parse::detail {

// Combine two source ranges into the smallest range covering both. An
// invalid endpoint is treated as absent, so merging with one yields the
// other. Used to stamp each AST node with the span of its constituent
// tokens.
inline diag::SourceRange merge(diag::SourceRange a, diag::SourceRange b) {
    if (!a.is_valid()) {
        return b;
    }
    if (!b.is_valid()) {
        return a;
    }
    auto end = b.begin.offset + b.length;
    return {a.begin, end - a.begin.offset};
}

}  // namespace vestra::parse::detail
