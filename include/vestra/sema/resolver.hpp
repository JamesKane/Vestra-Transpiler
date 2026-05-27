// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#pragma once

#include "vestra/ast/nodes.hpp"
#include "vestra/diag/diagnostic.hpp"
#include "vestra/sema/comptime.hpp"
#include "vestra/sema/scope.hpp"
#include "vestra/sema/types.hpp"

#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace vestra::sema {

// The result of name-resolving + type-checking a compilation unit.
//
// The side tables here are append-only: the resolver fills them and consumers
// (codegen, the future ownership checker) read them. AST nodes are not
// modified.
class Resolution {
public:
    [[nodiscard]] TypePtr type_of(const ast::Expr* e) const;
    [[nodiscard]] const Symbol* symbol_of(const ast::Expr* e) const;
    // §12.1 fold result — only expressions the comptime folder could
    // evaluate at compile time get an entry here. Codegen uses it to emit
    // a literal in place of the source expression.
    [[nodiscard]] const ComptimeValue* folded_value(const ast::Expr* e) const;
    // §12.6: was this decl skipped because an `@when(predicate)` on it
    // folded to false? Codegen consults this to keep its emission in
    // lockstep with sema.
    [[nodiscard]] bool is_gated_out(const ast::Decl* d) const;
    // §9 bare `catch NAME` — sema infers the error type from the
    // body's try-calls and stashes it here. Codegen reads this when
    // the DoCatchExpr's AST error_type is null.
    [[nodiscard]] TypePtr do_catch_error_type(const ast::DoCatchExpr* dc) const;

    void set_type(const ast::Expr* e, TypePtr t);
    void set_symbol(const ast::Expr* e, const Symbol* s);
    void set_folded_value(const ast::Expr* e, ComptimeValue v);
    void mark_gated_out(const ast::Decl* d);
    void set_do_catch_error_type(const ast::DoCatchExpr* dc, TypePtr t);

private:
    std::unordered_map<const ast::Expr*, TypePtr> expr_types_;
    std::unordered_map<const ast::Expr*, const Symbol*> expr_symbols_;
    std::unordered_map<const ast::Expr*, ComptimeValue> folded_;
    std::unordered_set<const ast::Decl*> gated_decls_;
    std::unordered_map<const ast::DoCatchExpr*, TypePtr> do_catch_error_;
};

// The resolver. Run `.resolve()` once; afterwards `resolution()` carries the
// findings and the reporter carries the diagnostics.
class Resolver {
public:
    Resolver(const ast::CompilationUnit& unit,
             TypeArena& types,
             diag::DiagnosticReporter& reporter,
             ComptimeFolder::EmbedReader embed_reader = {});

    Resolver(const Resolver&) = delete;
    Resolver& operator=(const Resolver&) = delete;

    void resolve();

    [[nodiscard]] const Resolution& resolution() const noexcept { return resolution_; }

private:
    // ---- pass 1: collect top-level decls ---------------------------------
    void register_builtin_capabilities();
    void register_builtin_math();
    void register_builtin_reflection();
    // §10 panic primitives: `panic(msg: Str) -> Never`, `abort() -> Never`,
    // `unreachable() -> Never`. Each is registered as a Func symbol so a
    // user call type-checks through the standard call path; the codegen
    // intercepts the three names and lowers to `__vstr_panic` /
    // `std::abort()` / `std::unreachable()` respectively.
    void register_builtin_panic();
    // §A5 (§14.10) sync-intrinsic builtins: nine free functions
    // (compilerFence, memoryBarrier, syncBarrier, instructionBarrier,
    // waitForInterrupt, waitForEvent, signalEvent, relax, nop).
    // Registered as Func symbols so user calls type-check normally;
    // codegen intercepts the names and lowers each to the matching
    // __vstr runtime shim. Depends on the Ordering / BarrierScope /
    // BarrierKind enums registered in register_builtin_reflection.
    void register_builtin_sync();
    void collect_top_level();
    void collect_func(const ast::FuncDecl& f);
    void collect_struct(const ast::StructDecl& s);
    void collect_enum(const ast::EnumDecl& e);
    void collect_protocol(const ast::ProtocolDecl& p);
    void collect_opaque(const ast::OpaqueDecl& o);
    void collect_const(const ast::ConstDecl& c);
    void collect_static(const ast::StaticDecl& s);

    // ---- pass 2: resolve bodies ------------------------------------------
    void check_decl(const ast::Decl& d);
    void check_func(const ast::FuncDecl& f);

    // §A1 (§6.7): validate shapes of the link-attribute family —
    // @section / @symbol / @alias / @weak / @noinit / @visibility.
    // No type-system effect; this just shapes the arguments so codegen
    // can lower them cleanly. Called from both Static and Func checks.
    void check_link_attributes(const std::vector<ast::Attribute>& attrs);

    void check_stmt(const ast::Stmt& s);

    // Compute and record an expression's type. `expected` is the type the
    // context wants — used for literal adaptation; pass nullptr when there
    // is no contextual type (the result then defaults).
    TypePtr check_expr(const ast::Expr& e, TypePtr expected = nullptr);
    TypePtr check_binary(const ast::BinaryExpr& b, TypePtr expected);
    TypePtr check_unary(const ast::UnaryExpr& u, TypePtr expected);
    TypePtr check_call(const ast::CallExpr& c, TypePtr expected = nullptr);
    TypePtr check_if(const ast::IfExpr& i, TypePtr expected);
    TypePtr check_match(const ast::MatchExpr& m, TypePtr expected);
    TypePtr check_block_expr(const ast::BlockExpr& b, TypePtr expected);
    TypePtr check_member(const ast::MemberExpr& m);
    TypePtr check_leading_dot(const ast::LeadingDotExpr& d, TypePtr expected);

    // Resolve an `ast::Type` node into a `sema::TypePtr`.
    TypePtr resolve_type(const ast::Type& t);

    // ---- struct / enum lookup helpers ------------------------------------

    // Look up a field by name on the struct type. Walks `embed` fields
    // recursively per §6 so embedded fields surface in the enclosing
    // namespace. `out_field` is populated with the resolved AST field when
    // the lookup succeeds. Returns nullptr if the field is not found.
    [[nodiscard]] TypePtr lookup_field(TypePtr struct_type,
                                       std::string_view name,
                                       const ast::StructDecl::Field** out_field = nullptr);

    // Look up a method by name on a struct/enum type. Returns the method's
    // full function type. `out_method` is set to the resolved FuncDecl.
    [[nodiscard]] TypePtr lookup_method(TypePtr owner_type,
                                        std::string_view name,
                                        const ast::FuncDecl** out_method = nullptr);

    // True when the unit has `derive(Proto, …) for T` for the given
    // nominal decl + protocol simple-name (last path segment). Used to
    // surface synthetic methods like `.clone()` from derive(Clone)
    // without materializing them in the AST.
    [[nodiscard]] bool decl_derives(const ast::Decl* decl, std::string_view protocol) const;

    // §6 tuple destructuring: walk a TuplePat in parallel with a
    // TupleType, binding each element pattern in the current scope to
    // the corresponding element's type. Reports arity / non-tuple
    // errors. Each leaf binding goes through the same IdentPat /
    // BindPat shape that LetStmt's single-name path uses.
    void bind_tuple_pattern(const ast::TuplePat& pat, TypePtr value_type);

    // §4 Display-conformance check: is a value of this type usable in
    // a `"\(...)"` interpolation splice? Primitives (numerics, Bool,
    // Char, string-likes) are always OK; user types must derive
    // Display or Debug (both lower to a std::formatter spec). A
    // vector is OK iff its element is OK.
    [[nodiscard]] bool is_display_conformant(TypePtr t) const;

    // §12.3 derive(Default): is `t` a type with a known zero / empty
    // value? Primitives (numerics, Bool, Str) have a natural zero;
    // Optional<T> has `nil`; a struct that derives Default is itself
    // Default-conformant (the protocol composes structurally). Box[T]
    // is *not* — allocation belongs in Box.new. Bare and payloaded
    // enums are skipped in v0.5 (no obvious default case).
    [[nodiscard]] bool is_default_conformant(TypePtr t) const;

    // Unify a generic parameter type against a concrete arg type, growing the
    // bindings map. A clash (same generic implied to be two different types
    // by different args) is reported at `site`.
    void unify_generic(TypePtr ptype,
                       TypePtr atype,
                       std::unordered_map<std::string, TypePtr>& bindings,
                       diag::SourceRange site);

    // Find an enum case by name on the given enum decl.
    [[nodiscard]] const ast::EnumDecl::Case* lookup_enum_case(const ast::EnumDecl& e,
                                                              std::string_view name);

    // The constructor type for an enum case: the nominal enum for a bare
    // case, or a function (T1, T2, ...) -> Enum for a payload case.
    [[nodiscard]] TypePtr enum_case_constructor_type(TypePtr enum_type,
                                                     const ast::EnumDecl::Case& c);

    // Pattern-typing for match arms. `scrutinee` is the type the arm sees.
    // Binds any `let`-pattern names into the current scope.
    void check_pattern(const ast::Pattern& p, TypePtr scrutinee);

    // Visibility check: report if `sym` is referenced from outside its
    // declaring scope when its visibility forbids it.
    void check_visibility(const Symbol& sym, diag::SourceRange use_range);

    // §12.6: returns true if any `@when(predicate)` attribute on `decl`
    // folds to Bool false. Gated-out decls are invisible to the rest of
    // sema and never reach codegen. A non-foldable predicate is treated
    // as live (we don't want a missing fold to silently delete code).
    [[nodiscard]] bool gated_out(const ast::Decl& decl);

    // ---- helpers ---------------------------------------------------------
    void error_at(diag::SourceRange r, std::string msg);
    void duplicate_definition(const Symbol& existing,
                              std::string_view name,
                              diag::SourceRange new_range);

    // Map a function decl to its full function type.
    TypePtr function_type_of(const ast::FuncDecl& f);

    const ast::CompilationUnit* unit_;
    TypeArena* types_;
    diag::DiagnosticReporter* reporter_;
    ScopeStack scopes_;
    Resolution resolution_;
    // Stack of expected return types — pushed when entering a function body so
    // a nested return expression can be checked against it. For a throws(E) →
    // T function we push the *success* type T here; the parallel throws_stack_
    // tracks E so `throw e` and `try f()` can reach it.
    std::vector<TypePtr> return_stack_;
    // §9 Stack of enclosing-function error types. Nullptr means "does not
    // throw" — `throw` is forbidden, propagating `try` is forbidden. A
    // non-null E means `throw e` requires e assignable to E, and `try f()`
    // requires f's error type assignable to E.
    std::vector<TypePtr> throws_stack_;
    // §9 bare `catch NAME`: when a do-block uses inferred-error-type
    // form, sema pushes a TypePtr* onto this stack pointing at the
    // do-catch's "discovered error" slot. A Propagating try inside
    // the body writes its callee's error type there (first try sets;
    // subsequent ones must match). Reset across function boundaries
    // so a do-catch nested inside a function-inside-a-do-catch
    // doesn't capture the outer one's inference.
    std::vector<TypePtr*> do_catch_infer_stack_;
    // §A10 (§15.5) the single @panic_handler this unit declares, if
    // any. Tracked at check_func time; the codegen reads it after
    // sema to decide whether to emit the runtime delegation block.
    const ast::FuncDecl* panic_handler_decl_ = nullptr;

    // §A4 (§14.9.3) `compareExchangeWeak` is admitted only inside one
    // of the recognized retry-loop shapes. We model that by bumping
    // this counter at the top of any while-stmt whose condition
    // matches an admitted shape (`!X.succeeded` or `true`); a weak-
    // CAS call site outside that range is rejected with a diagnostic
    // pointing at compareExchange instead. v0.5 doesn't yet handle
    // the `@retry_loop` function-level escape; users must spell out
    // the loop at the call site.
    int weak_cas_loop_depth_ = 0;
    // Comptime folder + the const environment it folds against. The env
    // accumulates name→value pairs as we successfully fold each top-level
    // const, so later consts can reference earlier ones. The folder gets
    // a pointer to the global scope so it can look comptime funcs up by
    // name when it encounters a call expression (§12.1 phase 2). The
    // EmbedReader (§12.1 phase 7) is supplied by the constructor and
    // routed straight through.
    ComptimeFolder folder_;
    ComptimeFolder::Env comptime_env_;

    // §12.2 reflection: synthetic `Type` + `Field` struct decls,
    // owned by the resolver so their lifetimes outlive every Symbol /
    // TypePtr that references them. Built once in
    // register_builtin_reflection() and never mutated thereafter.
    // Field declares fields in the order the folder expects them
    // in its ComptimeValue::elements vector ([0]=name, [1]=type).
    std::unique_ptr<ast::StructDecl> builtin_type_decl_;
    std::unique_ptr<ast::StructDecl> builtin_field_decl_;
    // §A4 (§14.9.1): Ordering carries the five memory-order cases.
    // Registered as a bare enum in global scope so `.relaxed` etc.
    // resolve through the existing leading-dot-case path.
    std::unique_ptr<ast::EnumDecl> builtin_ordering_decl_;
    // §A5 (§14.10.2): BarrierScope + BarrierKind for the CPU
    // memoryBarrier surface. Registered the same way as Ordering.
    std::unique_ptr<ast::EnumDecl> builtin_barrier_scope_decl_;
    std::unique_ptr<ast::EnumDecl> builtin_barrier_kind_decl_;
    // §A5 (§14.10.5): TlbScope for the TLB-invalidation surface.
    std::unique_ptr<ast::EnumDecl> builtin_tlb_scope_decl_;
};

}  // namespace vestra::sema
