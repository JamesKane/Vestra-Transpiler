// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#include "vestra/sema/linearity.hpp"

#include "vestra/ast/nodes.hpp"

namespace vestra::sema {

bool Linearity::is_linear(TypePtr t) {
    if (t == nullptr) {
        return false;
    }
    switch (t->kind()) {
    case TypeKind::Struct: {
        if (decl_is_linear(t->nominal_decl())) {
            return true;
        }
        // A generic struct instantiated over a linear argument owns it.
        for (auto p : t->parts()) {
            if (is_linear(p)) {
                return true;
            }
        }
        return false;
    }
    case TypeKind::Optional:
    case TypeKind::Box:
    case TypeKind::Vector:
    case TypeKind::Padded:
    case TypeKind::PerCpu:
        return is_linear(t->inner());
    case TypeKind::Result:
        return is_linear(t->inner()) || is_linear(t->result());
    case TypeKind::Vec:
    case TypeKind::HashMap:
    case TypeKind::Tuple:
        if (is_linear(t->inner())) {
            return true;
        }
        for (auto p : t->parts()) {
            if (is_linear(p)) {
                return true;
            }
        }
        return false;
    default:
        return false;
    }
}

bool Linearity::decl_is_linear(const ast::Decl* d) {
    if (d == nullptr || d->kind != ast::NodeKind::Struct) {
        return false;
    }
    if (auto it = memo_.find(d); it != memo_.end()) {
        return it->second == 1;
    }
    const auto& s = static_cast<const ast::StructDecl&>(*d);
    if (s.is_linear) {
        memo_[d] = 1;
        return true;
    }
    memo_[d] = 0;
    bool linear = false;
    if (arena_ != nullptr) {
        for (const auto& f : s.fields) {
            if (f.type != nullptr && is_linear(arena_->member_type(f.type.get()))) {
                linear = true;
                break;
            }
        }
    }
    memo_[d] = linear ? 1 : 2;
    return linear;
}

}  // namespace vestra::sema
