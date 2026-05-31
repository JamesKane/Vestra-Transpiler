// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#pragma once

// Internal helpers shared across the resolver translation units
// (resolver.cpp and its resolver_*.cpp siblings). These were the
// anonymous-namespace free functions that several check passes leaned on;
// hoisting them here as `inline` lets the file be split by theme without
// duplicating them. Not part of the public sema API — do not include
// outside src/sema/.

#include "vestra/ast/nodes.hpp"

#include <cstddef>
#include <vector>

namespace vestra::sema::detail {

// §A6 (§6.8 / §14.11.4) helper. Scans a struct's attributes for the
// `@repr(union)` marker so sema validation and (later) derive-target
// rejection share one detection point. Mirrors the codegen's
// `LayoutAttrs::is_union` reading.
inline bool struct_is_repr_union(const ast::StructDecl& s) {
    for (const auto& a : s.attributes) {
        if (a.name != "repr" || a.predicate == nullptr) {
            continue;
        }
        if (a.predicate->kind == ast::NodeKind::IdentExpr
            && static_cast<const ast::IdentExpr&>(*a.predicate).name == "union") {
            return true;
        }
    }
    return false;
}

// §7 generics phase 2 — count the named generic parameters of a struct or
// enum, type AND const (`[const N: Int]`) alike, since both consume a
// generic argument at a use site.
inline std::size_t named_type_param_count(const std::vector<ast::GenericParam>& gens) {
    std::size_t n = 0;
    for (const auto& g : gens) {
        if (!g.name.empty()) {
            ++n;
        }
    }
    return n;
}

}  // namespace vestra::sema::detail
