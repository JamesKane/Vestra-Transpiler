// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#pragma once

#include "vestra/ast/nodes.hpp"
#include "vestra/diag/diagnostic.hpp"
#include "vestra/sema/resolver.hpp"

#include <iosfwd>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace vestra::codegen {

// What the emitter writes for one Vestra source file.
struct EmittedUnit {
    std::string header;  // .hpp — types, function declarations
    std::string source;  // .cpp — function bodies, statics
};

// Translate a parsed Vestra compilation unit to C++26.
//
// The emitter is intentionally simple: it walks the AST and writes
// approximately what each construct would look like in idiomatic Modern C++.
// Coverage is the §13.x scope (primitives, control flow, simple funcs); more
// advanced constructs (capabilities, generics, ownership) emit a
// `#error "Vestra: unsupported construct — ..."` line plus a diagnostic so
// the gap is visible in both the build and the compiler log.
class CppEmitter {
public:
    // `resolution` is optional — when null, the emitter falls back to its
    // resolver-free behaviour (and so e.g. enum case access via `Color.red`
    // is emitted naively). When supplied, enum cases, member access, and
    // match scrutinees lower correctly.
    explicit CppEmitter(diag::DiagnosticReporter& reporter,
                        const sema::Resolution* resolution = nullptr)
        : reporter_(&reporter), resolution_(resolution) {}

    [[nodiscard]] EmittedUnit emit(const ast::CompilationUnit& unit,
                                   std::string_view output_basename);

private:
    void emit_decl(std::ostream& hdr, std::ostream& src, const ast::Decl& d);
    void emit_func(std::ostream& hdr, std::ostream& src, const ast::FuncDecl& f);
    void emit_struct(std::ostream& hdr, const ast::StructDecl& s);
    void emit_enum(std::ostream& hdr, const ast::EnumDecl& e);
    // §12.3 derive(Hash): emit a `template <> struct std::hash<Q::T>`
    // specialization after the user's namespace closes. `qual_prefix`
    // is the `Q::` part (or empty if the unit has no module decl).
    void emit_hash_spec(std::ostream& os, const ast::StructDecl& s, std::string_view qual_prefix);
    // §12.3 derive(Hash) for a payloaded enum. The body visits the
    // variant, seeds the hash with the alt index (so different cases
    // with equal payload bytes don't collide), and combines each
    // payload field via the same boost-style combine the struct path
    // uses. Bare enums don't need an emitted spec — the standard
    // library already provides `std::hash<E>` for enum types.
    void
    emit_hash_spec_enum(std::ostream& os, const ast::EnumDecl& e, std::string_view qual_prefix);
    // §12.3 derive(Debug): emit a `template <> struct std::formatter<Q::T>`
    // specialization at global scope so `std::format("{}", v)` /
    // `std::println("{}", v)` render the value structurally. Three
    // shapes — struct, bare enum class, sum-type-variant enum.
    void emit_debug_spec_struct(std::ostream& os,
                                const ast::StructDecl& s,
                                std::string_view qual_prefix);
    void
    emit_debug_spec_enum(std::ostream& os, const ast::EnumDecl& e, std::string_view qual_prefix);
    // §3 derive(Hash) / derive(Debug | Display) for an opaque newtype.
    // Both specs delegate to the underlying T's std::hash /
    // std::formatter, through a `static_cast<T>(q)`. Rendered shape is
    // `Q(value)` — keeps the nominal identity visible alongside the
    // underlying value.
    void
    emit_hash_spec_opaque(std::ostream& os, const ast::OpaqueDecl& o, std::string_view qual_prefix);
    void emit_debug_spec_opaque(std::ostream& os,
                                const ast::OpaqueDecl& o,
                                std::string_view qual_prefix);

    void emit_block(std::ostream& os, const ast::BlockExpr& b, int indent);
    void emit_stmt(std::ostream& os, const ast::Stmt& s, int indent);
    void emit_expr(std::ostream& os, const ast::Expr& e);
    void emit_match(std::ostream& os, const ast::MatchExpr& m);
    // §17.7 match-arm pattern helpers. `emit_pat_predicate` writes a C++
    // expression that returns true when the value spelled by `base`
    // matches the pattern; `emit_pat_bindings` writes the `auto&&
    // name = base;` lines for each binding the pattern introduces.
    // Used by the value-scrutinee if-chain and (predicate only) by the
    // bare-enum OrPat fallback.
    void emit_pat_predicate(std::ostream& os, const ast::Pattern& p, std::string_view base);
    void
    emit_pat_bindings(std::ostream& os, const ast::Pattern& p, std::string_view base, int indent);
    // Match over a non-enum scrutinee (integer, bool, string, tuple).
    // Lowers to an IIFE that binds the scrutinee to `__vstr_m` and
    // walks an if/else-if chain over each arm's predicate, with arm
    // bindings emitted inside the matching branch.
    void emit_match_value_scrutinee(std::ostream& os, const ast::MatchExpr& m);
    void emit_type(std::ostream& os, const ast::Type& t);
    // Emit the C++ spelling of a resolver-canonicalized type. Used in a
    // handful of places where the AST type isn't reachable (e.g. the
    // success-T slot of `std::expected<T, E>` for `do { ... } catch
    // (...) { ... }` — T is the do-catch expression's inferred result
    // type, not anything written in the source).
    void emit_sema_type(std::ostream& os, sema::TypePtr t);

    // §9 stmt-position lowering for expressions whose runtime semantics
    // need a statement (a real `return`, an `if/else` chain, etc.). When
    // `return_value` is true the trailing expression of `expr` is the
    // returned value; otherwise it's a discarded side-effect. Handles
    // ThrowExpr, TryExpr::Propagating, IfExpr, BlockExpr, MatchExpr,
    // and falls back to plain `<expr>;` / `return <expr>;` otherwise.
    void emit_stmt_expr(std::ostream& os, const ast::Expr& expr, bool return_value);

    // §9 mid-expression `try` hoisting. A `try EXPR` only escapes
    // cleanly at a statement boundary (a real `return std::unexpected
    // {...};`), so before emitting a statement we walk its expression
    // tree, register every TryExpr-Propagating sub-node, and pre-emit
    // each one as a let-binding of the unwrapped value. emit_expr
    // substitutes the binding name when it later hits the registered
    // TryExpr. The walk skips conditional contexts (IfExpr/MatchExpr
    // branches, short-circuit && / ||) so a hoisted try never runs in a
    // branch that the source program wouldn't have taken.
    //
    // The same machinery is overloaded to hoist a whole IfExpr whose
    // branches contain a propagating try: the conditional becomes a
    // lambda returning `std::expected<T, E>`, and the outer scope
    // unwraps it via the same `if (!r) return std::unexpected{r.error()};`
    // pattern. `node` is widened to `ast::Expr*` to cover that case.
    struct TryHoist {
        const ast::Expr* node;
        std::string name;
    };
    void collect_try_hoists(const ast::Expr& e,
                            std::vector<TryHoist>& out,
                            const std::vector<TryHoist>* exclude = nullptr);
    void collect_stmt_hoists(const ast::Stmt& s, std::vector<TryHoist>& out);
    void emit_try_hoist(std::ostream& os, const TryHoist& h, int indent);
    // Conditional hoist: IfExpr or MatchExpr containing a propagating
    // try. Emits a lambda returning std::expected<T, E> over the body
    // plus the outer propagation check.
    void emit_cond_hoist(std::ostream& os, const TryHoist& h, int indent);
    // §9 statement-form match used inside the conditional hoist's
    // lambda body. Mirrors emit_match's shape (switch for bare enum,
    // if-chain over std::holds_alternative for payloaded enum) but
    // emits each arm body via emit_stmt_expr(..., return_value=true)
    // so propagating tries escape with `return std::unexpected{...}`
    // from the enclosing cond-hoist lambda. No outer IIFE here — the
    // lambda envelope is supplied by emit_cond_hoist.
    void emit_match_in_lambda(std::ostream& os, const ast::MatchExpr& m, int indent);
    const std::string* lookup_try_hoist(const ast::Expr* node) const;

    // True when any sub-expression of `e` (walking into IfExpr branches,
    // MatchExpr arms, BlockExpr trailing positions, …) is a propagating
    // `try`. Used by collect_try_hoists to decide whether to register a
    // conditional hoist for a given IfExpr/MatchExpr.
    [[nodiscard]] bool expr_contains_propagating_try(const ast::Expr& e) const;
    [[nodiscard]] bool stmt_contains_propagating_try(const ast::Stmt& s) const;

    const std::vector<TryHoist>* active_hoists_ = nullptr;
    int hoist_counter_ = 0;
    // §9 self-suppression: while emit_cond_hoist is emitting the body
    // of an IfExpr's IIFE, the IfExpr is its own hoist target — we
    // must not substitute `*name` for it during that emission.
    // lookup_try_hoist consults this and returns nullptr for skip_hoist_.
    const ast::Expr* skip_hoist_ = nullptr;
    // §9 propagation target: the enclosing function's `throws(E)` E type
    // (the AST node — codegen renders it via emit_type). Set at the top
    // of emit_func and restored after; emit_cond_hoist writes it into
    // the lambda's `std::expected<T, E>` return type.
    const ast::Type* current_throws_type_ = nullptr;

    // §6 nested tuple destructuring. C++ structured bindings are
    // single-level, so `let ((a, b), c) = …` needs a sibling-statement
    // hoist: a placeholder name binds the inner tuple in the outer
    // structured binding, then a follow-on `auto [a, b] = placeholder;`
    // unpacks it. `collect_tuple_pat_names` walks one TuplePat level,
    // appending one entry to `names` per element (the real binder name
    // for leaf patterns, a generated placeholder for nested tuple
    // sub-patterns) and recording each nested sub-tuple in `followons`.
    // `emit_tuple_pat_followons` then emits each follow-on as
    // `auto [...] = placeholder;`, recursing for deeper nesting.
    void
    collect_tuple_pat_names(const ast::TuplePat& tp,
                            std::vector<std::string>& names,
                            std::vector<std::pair<std::string, const ast::TuplePat*>>& followons);
    void emit_tuple_pat_followons(
        std::ostream& os,
        const std::vector<std::pair<std::string, const ast::TuplePat*>>& followons,
        int indent);
    int tuple_pat_counter_ = 0;

    void unsupported(std::ostream& os, std::string_view what, diag::SourceRange r);

    static const char* binop_text(ast::BinaryOp op);
    static const char* unop_text(ast::UnaryOp op);

    diag::DiagnosticReporter* reporter_;
    const sema::Resolution* resolution_ = nullptr;

    // §12.3 derive layer: a target-type-name → derived-protocol-name
    // index built up front from every `derive(...) for T` top-level
    // decl in the unit. The struct/enum emitters consult this so
    // e.g. `derive(Eq) for Point` injects a defaulted `operator==`
    // into Point's body. Reset each emit() call.
    std::unordered_map<std::string, std::unordered_set<std::string>> derives_by_target_;
};

}  // namespace vestra::codegen
