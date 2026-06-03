// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#pragma once

#include "vestra/ast/nodes.hpp"
#include "vestra/sema/scope.hpp"
#include "vestra/sema/types.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace vestra::sema {

// A constant value produced by §12.1 comptime evaluation. The smallest set
// that covers the §12 examples a phase 1 evaluator actually needs to fold:
// integers, floats, bool, and a marker "unit" (the value of a block whose
// last statement isn't an expression).
//
// Integers are kept as a single signed 64-bit slot for simplicity — every
// integer literal in the supported range fits. The TypeKind that the value
// was *folded against* lives alongside in ComptimeValue::type so a later
// rendering pass knows the destination width.
struct ComptimeValue {
    // NOLINTNEXTLINE(performance-enum-size)
    enum class Kind : std::uint8_t {
        Int,
        UInt,
        Float,
        Bool,
        String,
        Vector,
        Field,
        TypeRef,
        Unit,
        // §12.4 an AST value: what `quote { … }` folds to inside a macro body,
        // and the `Decl` reflection parameter. `code` points at the node (a
        // cloned, owned subtree for a quote result; a non-owning alias for the
        // annotated decl). Materialized back into the program via ast::clone.
        Code,
    };

    Kind kind = Kind::Unit;
    TypeKind type = TypeKind::Unit;  // destination Vestra type. For a Vector
                                     // this is the *element* TypeKind; the
                                     // length lives in `length` below.

    std::int64_t i = 0;   // valid when Kind::Int
    std::uint64_t u = 0;  // valid when Kind::UInt
    double f = 0.0;       // valid when Kind::Float
    bool b = false;       // valid when Kind::Bool
    std::string s;        // valid when Kind::String — also the carrier for
                          // §12.6 enum-case identities (cfg.arch returns
                          // "arm64"; `.arm64` leading-dot folds to the
                          // same; equality is string compare). Also the
                          // name slot for Kind::Field and Kind::TypeRef
                          // (each reflective value is a labeled struct
                          // shape, named-field access dispatches by
                          // the Field/Type built-in StructDecl).

    // Valid when Kind::Vector. `elements.size() == length` after
    // construction; we still carry `length` separately because some
    // construction paths (e.g. `.zero` with a `[N]T` annotation) want
    // the length pinned independently before elements are populated.
    //
    // Also reused for Kind::Field as a positional members vector,
    // indexed in the same order as the synthetic Field StructDecl's
    // fields ([0]=name, [1]=type, ...). MemberExpr dispatch walks the
    // StructDecl to find the member's index and reads elements[idx].
    // Field StructDecl details belong to the resolver; the folder
    // looks the decl up by name via its global_scope_.
    std::vector<ComptimeValue> elements;
    std::int64_t length = 0;

    // Valid when Kind::Code (§12.4). Shared so ComptimeValue stays copyable as
    // it flows through folder frames; `const` because reflection reads the AST
    // and a quote result is never mutated in place — materialization clones it.
    std::shared_ptr<const ast::Node> code;

    // Render this value as a literal expression suitable to drop into C++
    // source. Picks an explicit-width form when `type` is set (e.g. `42`
    // for Int32, `42ULL` for UInt64, `1.5f` for Float32). For vectors
    // emits the brace-init pair `{{e0, e1, ..., eN-1}}` that slots into
    // the surrounding const's `std::array<T, N>` type annotation.
    [[nodiscard]] std::string to_cpp_literal() const;
};

class Resolution;  // forward — defined in resolver.hpp

// Pure-expression folder for §12.1 comptime evaluation. Walks an
// expression tree and returns the resulting value, or std::nullopt if the
// expression is not foldable in phase 1 (it called a function, read a
// non-folded variable, performed I/O, etc.).
//
// What this folder evaluates today
//   * Integer / float / bool / char literals (literally — char folds to
//     its UInt32 code-point representation only when needed).
//   * IdentExpr — looks the symbol up in the supplied environment; we
//     only fold lookups into already-folded `const` decls and Trivial
//     primitives (no Locals).
//   * Unary: -, !, ~
//   * Binary: arithmetic (+ - * / %), bitwise (& | ^ << >>),
//     comparisons (== != < <= > >=), logical (&& || — short-circuits),
//     coalesce (??) only when the LHS is non-Optional (i.e. trivially
//     non-null at fold time — phase 1 has no optional literals).
//   * `if` expression — folds the chosen branch.
//   * `comptime { ... }` and `comptime expr` — folds the wrapped body.
//   * BlockExpr — folds the trailing expression, treats other stmts as
//     non-foldable (since we don't model side effects).
//   * ParenExpr — recurses.
//
// What it deliberately doesn't fold
//   * Calls, struct construction, member access, indexing — phase 1.
//   * Optionals, strings, tuples, vectors, structs.
//   * Side-effecting expressions (await, spawn, throw, copy of move-only).
//   * Match, closures, try.
//
// Overflow / div-by-zero: detected and reported as a non-fold (returns
// nullopt). The caller is welcome to fall back to runtime evaluation —
// phase 1 doesn't treat unfolded as an error, just as "no constant value
// known."
class ComptimeFolder {
public:
    // Environment of names that are already known to be compile-time
    // constants — typically other `const` decls in the same compilation
    // unit. Lookups are by name (the resolver's symbol identity isn't
    // needed for this scoped fold).
    using Env = std::unordered_map<std::string, ComptimeValue>;

    // Recursion depth cap. Picked generously enough for hand-written code
    // (factorial(64), an Ackermann-style derivation chain) but small
    // enough that a runaway fold gives up fast instead of stack-overflowing
    // the compiler.
    static constexpr int MaxDepth = 64;

    // Per-loop iteration cap. Picked to comfortably handle the §12.1
    // sin_table case (256 iterations) and a generous margin for hand-
    // written tables, while still catching runaway loops fast.
    static constexpr int MaxLoopIterations = 1'000'000;

    // §12.1 `@embed("path")` callback. The folder calls this with the
    // raw path string from the source; the implementation resolves it
    // (against the source-file directory, eventually against a content-
    // hashed manifest) and returns the bytes, or std::nullopt on any
    // failure. Pass nullptr to disable @embed (calls fail to fold).
    using EmbedReader =
        std::function<std::optional<std::vector<std::uint8_t>>(std::string_view path)>;

    // `global_scope` is consulted when the folder encounters a call: it
    // looks the callee up by name, checks for a comptime func, and
    // recursively folds its body. Pass nullptr in unit-test contexts that
    // only need pure-expression folding. `embed_reader` similarly: nullptr
    // disables @embed.
    explicit ComptimeFolder(const Scope* global_scope = nullptr, EmbedReader embed_reader = {})
        : global_scope_(global_scope), embed_reader_(std::move(embed_reader)) {}

    // Fold `e` against `env`. If `hint` is non-Unit and the expression is
    // an integer/float literal, the result's `type` field uses the hint
    // instead of the literal default. `depth` is the current recursion
    // depth from any enclosing comptime function call; callers should
    // normally leave it at 0.
    [[nodiscard]] std::optional<ComptimeValue>
    fold(const ast::Expr& e, const Env& env, TypeKind hint = TypeKind::Unit, int depth = 0) const;

    // §12.4 run a declaration macro: fold `macro`'s body with its `Decl`
    // parameter bound to `annotated`, returning the declarations it produces
    // (the materialized `[Decl]` quote result), or std::nullopt if the body
    // didn't fold to a declaration list. Owned, freshly-cloned nodes ready to
    // splice into the unit. `quote { … }` folds to a Code value; splices
    // (`$d`, `$(d.name)`, `$(expr)`) fold their inner and materialize it.
    [[nodiscard]] std::optional<std::vector<ast::DeclPtr>>
    expand_decl_macro(const ast::FuncDecl& macro, const ast::Decl& annotated) const;

private:
    // Per-call mutable state. A new Frame is built at the entry of every
    // comptime function call (so callees can't see the caller's locals)
    // and at the entry of every top-level fold (so unit tests that pass
    // only an Env get a fresh empty frame).
    struct Frame {
        Env locals;
        std::optional<ComptimeValue> returned;  // set by a ReturnStmt;
                                                // BlockExpr stops on this
    };

    // Recursive fold. The public fold() constructs a Frame and calls this.
    [[nodiscard]] std::optional<ComptimeValue>
    fold_with(const ast::Expr& e, const Env& env, Frame& frame, TypeKind hint, int depth) const;

    // Statement-level fold. Returns true when the statement walked without
    // a bail; false on any unfoldable shape. Effects accumulate in frame.
    [[nodiscard]] bool fold_stmt(const ast::Stmt& s, const Env& env, Frame& frame, int depth) const;

    // §12.4 macro evaluation. `materialize_value` turns a folded scalar /
    // Code value into an AST expression (Int → IntLit, String → StringLit, …,
    // Code → clone). `subst_splices` walks a cloned quote body and replaces
    // each `$(expr)` splice by folding the inner and materializing it.
    [[nodiscard]] ast::ExprPtr materialize_value(const ComptimeValue& v) const;
    void subst_splices(ast::ExprPtr& e, const Env& env, Frame& frame, int depth) const;
    void subst_splices_in_stmt(ast::Stmt& s, const Env& env, Frame& frame, int depth) const;
    // §12.4 like subst_splices, but for a child in identifier position (a call
    // callee or a member/index base): a splice folding to a String becomes an
    // identifier *reference* (`$(h)()` calls the function named by `h`) rather
    // than a string literal. Non-string values fall back to materialize_value.
    void subst_splices_ident(ast::ExprPtr& e, const Env& env, Frame& frame, int depth) const;

    const Scope* global_scope_ = nullptr;
    EmbedReader embed_reader_;

    // §12.4 hygiene: monotonic counter behind the `gensym()` builtin. One
    // folder is reused across all declaration-macro expansions in a unit, so
    // the counter is unique unit-wide. `mutable` because folding is otherwise
    // const and gensym is the one fold that carries state between calls.
    mutable std::int64_t gensym_counter_ = 0;
};

}  // namespace vestra::sema
