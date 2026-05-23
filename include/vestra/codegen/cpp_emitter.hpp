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
    // §12.3 derive(Debug): emit a `template <> struct std::formatter<Q::T>`
    // specialization at global scope so `std::format("{}", v)` /
    // `std::println("{}", v)` render the value structurally. Three
    // shapes — struct, bare enum class, sum-type-variant enum.
    void emit_debug_spec_struct(std::ostream& os,
                                const ast::StructDecl& s,
                                std::string_view qual_prefix);
    void
    emit_debug_spec_enum(std::ostream& os, const ast::EnumDecl& e, std::string_view qual_prefix);

    void emit_block(std::ostream& os, const ast::BlockExpr& b, int indent);
    void emit_stmt(std::ostream& os, const ast::Stmt& s, int indent);
    void emit_expr(std::ostream& os, const ast::Expr& e);
    void emit_match(std::ostream& os, const ast::MatchExpr& m);
    void emit_type(std::ostream& os, const ast::Type& t);

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
    struct TryHoist {
        const ast::TryExpr* node;
        std::string name;
    };
    void collect_try_hoists(const ast::Expr& e, std::vector<TryHoist>& out);
    void collect_stmt_hoists(const ast::Stmt& s, std::vector<TryHoist>& out);
    void emit_try_hoist(std::ostream& os, const TryHoist& h, int indent);
    const std::string* lookup_try_hoist(const ast::TryExpr* node) const;

    const std::vector<TryHoist>* active_hoists_ = nullptr;
    int hoist_counter_ = 0;

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
