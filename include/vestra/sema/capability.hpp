#pragma once

#include "vestra/ast/nodes.hpp"
#include "vestra/diag/diagnostic.hpp"
#include "vestra/diag/source_manager.hpp"
#include "vestra/sema/resolver.hpp"

#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace vestra::sema {

// Capability resolution — phase 1 of §8 and §19.7.
//
// Each function declares the capabilities it consumes in its `using` row.
// At every call site, every capability the callee requires must be
// supplied by the caller — either because the caller declares the same
// row, or because an enclosing `with X = value { ... }` block provides
// it lexically. `async func` adds an implicit `Async` to the row;
// `await` and `spawn` require `Async` to be in scope at the use site.
//
// What this pass deliberately does not (yet) do
//   * Type-check the value bound in `with X = value`. Phase 2 will verify
//     the value is convertible to X.
//   * Row polymorphism over a generic capability variable `C` in higher-
//     order signatures.
//   * Capability narrowing (`Mmio.narrowed(to:)` etc.).
//   * Unsafe-op gating — `Ptr.load()` needs `RawMemory`, an `asm` block
//     needs `Asm`, an `@extern("C")` call needs `Extern`. We have no
//     built-in operations of any of those kinds yet.
//   * Capability used as a value (`Log.info("...")`).
//
// Capabilities are tracked by name (the first segment of the type's path):
// the §8 model treats capability identity as nominal, not structural, and
// every standard capability is registered as a built-in symbol so the
// matching is sound for the names in §8.
class CapabilityChecker {
public:
    CapabilityChecker(const ast::CompilationUnit& unit,
                      const Resolution& resolution,
                      diag::DiagnosticReporter& reporter)
        : unit_(&unit), resolution_(&resolution), reporter_(&reporter) {}

    CapabilityChecker(const CapabilityChecker&) = delete;
    CapabilityChecker& operator=(const CapabilityChecker&) = delete;

    void check();

private:
    void check_decl(const ast::Decl& d);
    void check_func(const ast::FuncDecl& f);
    void check_stmt(const ast::Stmt& s);
    void check_expr(const ast::Expr& e);

    // The capability names required by `callee` (its `using` row, plus
    // an implicit "Async" if it's an async function). Returned by value
    // because each row is small.
    [[nodiscard]] std::vector<std::string> required_caps(const ast::FuncDecl& callee) const;

    // First segment of an ast::Type's path. Empty if the type isn't a
    // NamedType (or has no segments) — those don't name capabilities.
    [[nodiscard]] static std::string cap_name(const ast::Type& t);

    [[nodiscard]] bool in_scope(std::string_view name) const noexcept;

    // Report a missing capability at `site`.
    void missing_capability(std::string_view name, diag::SourceRange site);

    const ast::CompilationUnit* unit_;
    const Resolution* resolution_;
    diag::DiagnosticReporter* reporter_;

    // Stack of capability scopes. Each layer is the set of names added by
    // the enclosing function entry or a `with` block.
    std::vector<std::unordered_set<std::string>> scopes_;
};

}  // namespace vestra::sema
