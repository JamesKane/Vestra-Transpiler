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
    assert(scopes_.size() > 1 && "cannot pop the global scope");
    auto* gone = scopes_.back().get();
    active_ = gone->parent();
    scopes_.pop_back();
}

}  // namespace vestra::sema
