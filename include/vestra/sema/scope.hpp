// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#pragma once

#include "vestra/ast/nodes.hpp"
#include "vestra/diag/source_manager.hpp"
#include "vestra/sema/types.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace vestra::sema {

// NOLINTNEXTLINE(performance-enum-size)
enum class SymbolKind : std::uint8_t {
    Func,
    Struct,
    Enum,
    EnumCase,
    Protocol,
    Const,
    Static,
    OpaqueType,
    Local,  // let / var
    Parameter,
    GenericParam,
};

struct Symbol {
    std::string name;
    SymbolKind kind;
    const ast::Node* decl = nullptr;  // the AST node this symbol points at
    TypePtr type = nullptr;           // computed type, when known
    diag::SourceRange definition_range{};
    // Visibility level recorded for top-level decls. For locals/params the
    // value is meaningless and stays at the default.
    ast::Visibility visibility = ast::Visibility::Internal;
    // §5/§18.4 partition provenance. When this binding is a sub-view derived
    // from a parent place — a `split(at:)` half or a `chunks(of:)` iterator —
    // `provenance_root` is the parent's Symbol and `provenance_segment` is a
    // synthetic discriminator ("@split0", "@split1", "@chunks"). The
    // exclusivity checker maps the sub-view to a place rooted at the parent
    // with that segment, so a borrow of the sub-view conflicts with a borrow
    // of the parent, while disjoint split halves (distinct segments) do not
    // conflict with each other. Stored on the Symbol (not a side table) so the
    // resolver writer and the exclusivity reader reach it through the
    // identical, scope-stable object.
    const Symbol* provenance_root = nullptr;
    std::string provenance_segment;
};

// A lexical scope. The owner uses `enter()` / `leave()` (or the RAII
// ScopeGuard) to push/pop scopes around blocks, functions, structs, etc.
class Scope {
public:
    explicit Scope(Scope* parent = nullptr) : parent_(parent) {}
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    Scope(Scope&&) = default;
    Scope& operator=(Scope&&) = default;
    ~Scope() = default;

    // Insert a symbol. Returns nullptr on success; on duplicate, returns the
    // existing symbol so the caller can include its definition site in the
    // diagnostic.
    [[nodiscard]] const Symbol* insert(Symbol s);

    // Look up by name, walking parents.
    [[nodiscard]] const Symbol* lookup(std::string_view name) const;

    // Mutable lookup, walking parents — used to annotate a binding in place
    // (e.g. §5/§18.4 partition provenance) after it has been inserted.
    [[nodiscard]] Symbol* lookup_mutable(std::string_view name);

    // Look up in this scope only.
    [[nodiscard]] const Symbol* lookup_local(std::string_view name) const;

    [[nodiscard]] Scope* parent() const noexcept { return parent_; }

private:
    Scope* parent_;
    std::unordered_map<std::string, Symbol> symbols_;
};

// A stack of scopes. ScopeStack owns every scope it creates so callers never
// have to manage Scope lifetimes.
class ScopeStack {
public:
    ScopeStack();
    ScopeStack(const ScopeStack&) = delete;
    ScopeStack& operator=(const ScopeStack&) = delete;
    ScopeStack(ScopeStack&&) = default;
    ScopeStack& operator=(ScopeStack&&) = default;
    ~ScopeStack() = default;

    [[nodiscard]] Scope& current() noexcept { return *active_; }
    [[nodiscard]] Scope& global() noexcept { return *scopes_.front(); }

    Scope& push();
    void pop();

    // RAII helper — pushes on construction, pops on destruction.
    class Guard {
    public:
        explicit Guard(ScopeStack& s) : stack_(&s), scope_(&s.push()) {}
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;
        Guard(Guard&&) = delete;
        Guard& operator=(Guard&&) = delete;
        ~Guard() { stack_->pop(); }

        [[nodiscard]] Scope& scope() const noexcept { return *scope_; }

    private:
        ScopeStack* stack_;
        Scope* scope_;
    };

private:
    std::vector<std::unique_ptr<Scope>> scopes_;
    Scope* active_ = nullptr;
};

}  // namespace vestra::sema
