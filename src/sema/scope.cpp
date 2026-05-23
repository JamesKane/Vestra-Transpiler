// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#include "vestra/sema/scope.hpp"

#include <cassert>
#include <memory>
#include <string>
#include <utility>

namespace vestra::sema {

// ----- Scope -------------------------------------------------------------

const Symbol* Scope::insert(Symbol s) {
    auto [it, inserted] = symbols_.try_emplace(s.name, std::move(s));
    if (!inserted) {
        return &it->second;
    }
    return nullptr;
}

const Symbol* Scope::lookup(std::string_view name) const {
    for (const auto* s = this; s != nullptr; s = s->parent_) {
        if (auto* found = s->lookup_local(name)) {
            return found;
        }
    }
    return nullptr;
}

const Symbol* Scope::lookup_local(std::string_view name) const {
    auto it = symbols_.find(std::string{name});
    return it == symbols_.end() ? nullptr : &it->second;
}

// ----- ScopeStack --------------------------------------------------------

ScopeStack::ScopeStack() {
    scopes_.push_back(std::make_unique<Scope>(nullptr));
    active_ = scopes_.front().get();
}

Scope& ScopeStack::push() {
    scopes_.push_back(std::make_unique<Scope>(active_));
    active_ = scopes_.back().get();
    return *active_;
}

void ScopeStack::pop() {
    // We intentionally do NOT destroy the popped scope. Symbols inserted into
    // a scope are referenced by pointer from the Resolution side table — and
    // codegen, which runs after sema, follows those pointers. Keeping every
    // scope alive for the ScopeStack's lifetime is the simplest way to make
    // those pointers stable; the memory cost is bounded by the source's total
    // nesting (small in practice).
    assert(active_ != nullptr && active_->parent() != nullptr && "cannot pop the global scope");
    active_ = active_->parent();
}

}  // namespace vestra::sema
