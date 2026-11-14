// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#pragma once

// §19.4 escapability. A non-escapable value (a Span, MutSpan or Str, or a
// struct holding one) is derived from a source, and may not outlive it. This
// pass infers each view's sources from the expressions that produce it and
// checks them where a view leaves its scope: a `return` (or a body's trailing
// value) may not hand back a view of the function's own storage, of a
// temporary, or of memory minted under a `with RawMemory` / `with Mmio` block
// inside the function. A Future can't be returned at all (§11.1).

#include "vestra/ast/nodes.hpp"
#include "vestra/diag/diagnostic.hpp"
#include "vestra/sema/resolver.hpp"

#include <unordered_map>
#include <vector>

namespace vestra::sema {

class EscapeChecker {
public:
    EscapeChecker(const ast::CompilationUnit& unit,
                  const Resolution& resolution,
                  diag::DiagnosticReporter& reporter)
        : unit_(&unit), resolution_(&resolution), reporter_(&reporter) {}

    EscapeChecker(const EscapeChecker&) = delete;
    EscapeChecker& operator=(const EscapeChecker&) = delete;
    EscapeChecker(EscapeChecker&&) = delete;
    EscapeChecker& operator=(EscapeChecker&&) = delete;
    ~EscapeChecker() = default;

    void check();

private:
    // Where a view's memory lives.
    struct Origin {
        enum class Kind : std::uint8_t { Local, Temp, Raw, Mmio } kind;
        const Symbol* sym = nullptr;  // Local: the binding viewed
        diag::SourceRange block{};    // Raw / Mmio: the `with` block (invalid: the caller's)
        int depth = 0;                // Local: the binding's block depth; Raw / Mmio: the block's
    };
    using Origins = std::vector<Origin>;

    void check_func(const ast::FuncDecl& f);
    void check_stmt(const ast::Stmt& s);
    void check_expr(const ast::Expr& e);
    void check_return(const ast::Expr& value, diag::SourceRange site);
    // §19.4 storing a view into `target` (a binding at block depth `depth`)
    // that would outlive one of its sources.
    void check_store(const Symbol* target, int depth, const Origins& o, diag::SourceRange site);

    // The sources of `e`, a value flowing into a view-typed slot: a view
    // carries its own sources; a non-view value coerced into one is a view
    // of that storage.
    [[nodiscard]] Origins origins(const ast::Expr& e) const;
    [[nodiscard]] Origins storage_of(const ast::Expr& e) const;

    [[nodiscard]] bool is_view(TypePtr t, int depth = 0) const;
    [[nodiscard]] bool ast_is_view(const ast::Type* t, int depth = 0) const;
    [[nodiscard]] const ast::StructDecl* unit_struct(std::string_view name) const;

    const ast::CompilationUnit* unit_;
    const Resolution* resolution_;
    diag::DiagnosticReporter* reporter_;

    const ast::FuncDecl* func_ = nullptr;
    bool result_is_view_ = false;
    std::unordered_map<const Symbol*, Origins> locals_;
    // The `with RawMemory` / `with Mmio` blocks around the current point.
    std::vector<diag::SourceRange> raw_blocks_;
    std::vector<diag::SourceRange> mmio_blocks_;
    std::vector<int> raw_depths_;   // the block depth of each `with` body above
    std::vector<int> mmio_depths_;
    // Block nesting: the function body is depth 1.
    int depth_ = 0;
    std::unordered_map<const Symbol*, int> decl_depth_;
};

}  // namespace vestra::sema
