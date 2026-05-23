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
    Module,
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
    Field,
    Method,
};

struct Symbol {
    std::string name;
    SymbolKind kind;
    const ast::Node* decl = nullptr;  // the AST node this symbol points at
    TypePtr type = nullptr;           // computed type, when known
    diag::SourceRange definition_range{};
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
