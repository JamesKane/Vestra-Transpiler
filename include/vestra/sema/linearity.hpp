// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#pragma once

// §5 / §19.6 which types are linear. A `linear struct` is; so, transitively,
// is anything that owns one: a struct with a linear field, and an owning
// container over a linear element (Optional, Box, Vec, HashMap, a fixed
// array, a tuple, Result, Padded, PerCpu). Views (Span, Ptr, ...) don't own
// and so don't propagate it.
//
// A struct's field types come from the build's shared arena
// (TypeArena::member_type), which each module records as it resolves, so a
// struct imported from another module is judged by its own declaration.

#include "vestra/sema/types.hpp"

#include <unordered_map>

namespace vestra::sema {

class Linearity {
public:
    explicit Linearity(const TypeArena* arena) : arena_(arena) {}

    [[nodiscard]] bool is_linear(TypePtr t);

private:
    [[nodiscard]] bool decl_is_linear(const ast::Decl* d);

    const TypeArena* arena_;
    // 0 = in progress (a recursive struct is not linear through itself),
    // 1 = linear, 2 = not linear.
    std::unordered_map<const ast::Decl*, int> memo_;
};

}  // namespace vestra::sema
