// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#pragma once

#include "vestra/diag/source_manager.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace vestra::ast {

// ------------------------------------------------------------------ enums

// Every concrete AST node has a unique NodeKind. This single discriminator
// lets us write visitors as switch statements without runtime type checks.
//
// NOLINTNEXTLINE(performance-enum-size)
enum class NodeKind : std::uint16_t {
    // ---- declarations ----
    Module,
    Import,
    Func,
    Struct,
    Enum,
    Protocol,
    Extension,
    Opaque,
    Const,
    Static,
    Derive,
    // ---- types ----
    NamedType,
    VectorType,
    OptionalType,
    FunctionType,
    TupleType,
    SomeType,
    DynType,
    // ---- statements ----
    LetStmt,
    VarStmt,
    ExprStmt,
    ReturnStmt,
    BreakStmt,
    ContinueStmt,
    ForStmt,
    WhileStmt,
    WithStmt,
    AssignStmt,
    // ---- expressions ----
    IntLit,
    FloatLit,
    StringLit,
    ByteStringLit,
    CharLit,
    BoolLit,
    NilLit,
    IdentExpr,
    PathExpr,
    SelfExpr,
    LeadingDotExpr,
    CallExpr,
    MemberExpr,
    IndexExpr,
    UnaryExpr,
    BinaryExpr,
    AsExpr,
    IfExpr,
    MatchExpr,
    SelectExpr,
    ClosureExpr,
    BlockExpr,
    ComptimeExpr,
    EmbedExpr,
    InterpStringExpr,
    QuoteExpr,
    TryExpr,
    DoCatchExpr,
    AwaitExpr,
    SpawnExpr,
    CopyExpr,
    ThrowExpr,
    VectorLitExpr,
    TupleLitExpr,
    ParenExpr,
    // ---- patterns ----
    WildcardPat,
    LiteralPat,
    BindPat,
    IdentPat,
    EnumPat,
    TuplePat,
    RangePat,
    OrPat,
};

// The four parameter modes from §5.
enum class ParamMode { Read, Inout, Sink };

// Visibility levels (§15). Internal is the default and so is the unset state.
enum class Visibility { Private, Internal, Package, Public };

// Unary operators usable in §17.7's prefix position. `Copy` is `copy expr`,
// but a separate UnaryOp::Copy keeps the AST flat (no special `CopyExpr` is
// strictly required, though we keep it for readability of consumers).
// `Unwrap` is the postfix `!` on an optional value (§9 force-unwrap, panics
// on `.none`). Distinct from `Not` because the operand type / lowering are
// completely different — Not requires Bool and emits `!x`; Unwrap requires
// Optional<T>, returns T, and emits `x.value()`.
enum class UnaryOp { Neg, Not, BitNot, Unwrap };

// Binary operators, ordered by precedence high→low to match §17.7.
enum class BinaryOp {
    Mul,
    Div,
    Mod,
    Add,
    Sub,
    Shl,
    Shr,
    BitAnd,
    BitXor,
    BitOr,
    Range,
    RangeLt,
    Eq,
    Ne,
    Lt,
    Le,
    Gt,
    Ge,
    And,
    Or,
    Coalesce,
    // wrapping arithmetic from §4
    WrappingAdd,
    WrappingSub,
    WrappingMul,
};

// Assignment kinds — §17.4 lists =, +=, -=, ...
enum class AssignOp {
    Assign,
    AddAssign,
    SubAssign,
    MulAssign,
    DivAssign,
    ModAssign,
    BitAndAssign,
    BitOrAssign,
    BitXorAssign,
    ShlAssign,
    ShrAssign,
};

// ------------------------------------------------------------------ bases

struct Node {
    NodeKind kind;
    diag::SourceRange range;

    explicit Node(NodeKind k) : kind(k) {}
    Node(const Node&) = delete;
    Node& operator=(const Node&) = delete;
    Node(Node&&) noexcept = default;
    Node& operator=(Node&&) noexcept = default;
    virtual ~Node() = default;
};

// Each grammar category gets its own base class so node pointers stay typed
// at every layer. The discriminator is still `kind`, so a switch on kind in
// a visitor is exhaustive over each category's concrete node set.
struct Decl : Node {
    using Node::Node;
};
struct Stmt : Node {
    using Node::Node;
};
struct Expr : Node {
    using Node::Node;
};
struct Type : Node {
    using Node::Node;
};
struct Pattern : Node {
    using Node::Node;
};

using DeclPtr = std::unique_ptr<Decl>;
using StmtPtr = std::unique_ptr<Stmt>;
using ExprPtr = std::unique_ptr<Expr>;
using TypePtr = std::unique_ptr<Type>;
using PatternPtr = std::unique_ptr<Pattern>;

// ------------------------------------------------------------------ shared

// An `@attribute(args)` annotation. We keep args as the raw token text for
// now; structured argument parsing lives in §12 metaprogramming and we'll
// flesh it out when we tackle macros.
struct Attribute {
    std::string name;
    // Single-expression argument: `@when(cfg.arch == .arm64)` puts the
    // comparison expression here; `@bits(4)` puts the integer literal.
    // Attributes that take no args leave this null. Phase 1 of §12.6 only
    // needs a single arg; multi-arg attributes can extend this to a vector.
    ExprPtr predicate;
    diag::SourceRange range;
};

// A `T: ProtocolA & ProtocolB` bound. `protocols` is the conjunction.
struct Bound {
    std::vector<TypePtr> protocols;
};

// A generic parameter. Either a type parameter (`T: Bound`) or a const
// parameter (`const N: Int`).
struct GenericParam {
    std::string name;
    bool is_const = false;
    TypePtr const_type;  // when is_const, this is the type of N
    Bound bound;         // when !is_const, optional bounds
    diag::SourceRange range;
};

// A function/struct/protocol parameter list element.
struct Param {
    // §17.2: `(IDENT|'_')? IDENT ':' 'nonescaping'? pmode? type ('=' expr)?`
    std::string label;           // external label; empty when omitted (defaults to name)
    bool label_omitted = false;  // true when `_` was written
    std::string name;
    bool nonescaping = false;
    ParamMode mode = ParamMode::Read;
    TypePtr type;
    ExprPtr default_value;  // optional
    // §6 tuple-pattern destructuring at the param slot:
    //   func f((a, b): (Int32, Int32)) -> Int32
    // When set, `name` is empty (the parser binds the pattern instead),
    // sema walks the pattern via bind_tuple_pattern, and codegen emits a
    // synthetic C++ parameter (`__vstr_arg_i`) plus follow-on
    // structured-binding statements at the top of the body.
    PatternPtr pattern;
    diag::SourceRange range;
};

// The `using Cap1, Cap2 throws(E)` effects clause from §17.2.
struct Effects {
    std::vector<TypePtr> using_caps;  // capabilities the function consumes
    TypePtr throws_type;              // null = does not throw
};

// ------------------------------------------------------------------ types

struct NamedType : Type {
    std::vector<std::string> path;  // dotted path: e.g. ["core", "math", "Matrix"]
    std::vector<TypePtr> type_args;
    // Pinned associated types — `[Element = UInt8]` — stored as (name, type)
    // pairs in `type_args` would lose the name; we keep them separate.
    std::vector<std::pair<std::string, TypePtr>> assoc_bindings;
    std::vector<std::int64_t> const_args;  // INT generic args
    bool has_generics = false;             // distinguishes `T` from `T[]` parsed as empty list

    NamedType() : Type(NodeKind::NamedType) {}
};

struct VectorType : Type {
    // `[N]T` where N is either a literal length or an IDENT (a const generic).
    std::int64_t length = 0;
    std::string length_ident;  // non-empty when length is symbolic
    TypePtr element;

    VectorType() : Type(NodeKind::VectorType) {}
};

struct OptionalType : Type {
    TypePtr inner;
    OptionalType() : Type(NodeKind::OptionalType) {}
};

struct FunctionType : Type {
    std::vector<TypePtr> params;
    Effects effects;
    TypePtr result;
    FunctionType() : Type(NodeKind::FunctionType) {}
};

struct TupleType : Type {
    std::vector<TypePtr> elements;
    TupleType() : Type(NodeKind::TupleType) {}
};

struct SomeType : Type {
    TypePtr inner;
    SomeType() : Type(NodeKind::SomeType) {}
};
struct DynType : Type {
    TypePtr inner;
    DynType() : Type(NodeKind::DynType) {}
};

// ------------------------------------------------------------------ exprs

struct IntLit : Expr {
    std::string text;
    IntLit() : Expr(NodeKind::IntLit) {}
};
struct FloatLit : Expr {
    std::string text;
    FloatLit() : Expr(NodeKind::FloatLit) {}
};
struct StringLit : Expr {
    std::string text;
    StringLit() : Expr(NodeKind::StringLit) {}
};
struct ByteStringLit : Expr {
    std::string text;
    ByteStringLit() : Expr(NodeKind::ByteStringLit) {}
};
struct CharLit : Expr {
    std::string text;
    CharLit() : Expr(NodeKind::CharLit) {}
};
struct BoolLit : Expr {
    bool value = false;
    BoolLit() : Expr(NodeKind::BoolLit) {}
};
// `nil` — §9 absence literal. Carries no payload; its type is whatever
// Optional<T> the context expects (and the resolver pins it there).
struct NilLit : Expr {
    NilLit() : Expr(NodeKind::NilLit) {}
};
struct SelfExpr : Expr {
    SelfExpr() : Expr(NodeKind::SelfExpr) {}
};
struct IdentExpr : Expr {
    std::string name;
    IdentExpr() : Expr(NodeKind::IdentExpr) {}
};
struct LeadingDotExpr : Expr {
    std::string name;
    LeadingDotExpr() : Expr(NodeKind::LeadingDotExpr) {}
};

struct PathExpr : Expr {
    std::vector<std::string> segments;
    PathExpr() : Expr(NodeKind::PathExpr) {}
};

struct CallExpr : Expr {
    ExprPtr callee;
    std::vector<TypePtr> type_args;
    // (label, is_inout, expr) — label empty if positional.
    struct Arg {
        std::string label;
        bool is_inout = false;
        ExprPtr value;
    };
    std::vector<Arg> args;
    CallExpr() : Expr(NodeKind::CallExpr) {}
};

struct MemberExpr : Expr {
    ExprPtr base;
    std::string member;
    // §9 optional chaining: `a?.b` peels one Optional layer from `a`
    // before looking up `b`, and wraps the lookup in Optional (.none
    // short-circuits the whole chain). Distinct AST shape from `a.b`
    // because the sema rules and lowering differ.
    bool is_optional_chain = false;
    MemberExpr() : Expr(NodeKind::MemberExpr) {}
};

struct IndexExpr : Expr {
    ExprPtr base;
    std::vector<ExprPtr> indices;
    std::vector<TypePtr> type_args;
    IndexExpr() : Expr(NodeKind::IndexExpr) {}
};

struct UnaryExpr : Expr {
    UnaryOp op;
    ExprPtr operand;
    UnaryExpr() : Expr(NodeKind::UnaryExpr) {}
};

struct BinaryExpr : Expr {
    BinaryOp op;
    ExprPtr lhs;
    ExprPtr rhs;
    BinaryExpr() : Expr(NodeKind::BinaryExpr) {}
};

struct AsExpr : Expr {
    ExprPtr value;
    TypePtr target;
    bool is_optional = false;  // `as?`
    AsExpr() : Expr(NodeKind::AsExpr) {}
};

struct IfExpr : Expr {
    // §9 `if let NAME = INIT { … }`: when `let_name` is non-empty, the
    // condition is implicit (`init` must produce an `Optional<T>`; if it
    // holds a value, the unwrapped `T` is bound to `let_name` in
    // `then_branch`'s scope and the branch runs; otherwise `else_branch`
    // runs). In that mode `cond` is null.
    std::string let_name;
    ExprPtr let_init;
    ExprPtr cond;
    ExprPtr then_branch;
    ExprPtr else_branch;  // optional; null when no `else`
    IfExpr() : Expr(NodeKind::IfExpr) {}
};

struct MatchArm {
    PatternPtr pattern;
    ExprPtr guard;  // optional `where`
    ExprPtr body;
    bool is_default = false;
};

struct MatchExpr : Expr {
    ExprPtr scrutinee;
    std::vector<MatchArm> arms;
    MatchExpr() : Expr(NodeKind::MatchExpr) {}
};

struct SelectArm {
    // `on let pattern = event :` — `pattern` is null when there is no binding.
    PatternPtr pattern;
    ExprPtr event;
    ExprPtr body;
};

struct SelectExpr : Expr {
    std::vector<SelectArm> arms;
    ExprPtr default_body;  // optional
    SelectExpr() : Expr(NodeKind::SelectExpr) {}
};

struct BlockExpr : Expr {
    std::vector<StmtPtr> stmts;
    BlockExpr() : Expr(NodeKind::BlockExpr) {}
};

struct ClosureExpr : Expr {
    std::vector<std::string> params;
    std::vector<StmtPtr> body;
    ClosureExpr() : Expr(NodeKind::ClosureExpr) {}
};

struct ComptimeExpr : Expr {
    ExprPtr inner;
    ComptimeExpr() : Expr(NodeKind::ComptimeExpr) {}
};
// `@embed("path")` — §12.1's sole non-pure comptime input. The folder
// reads the named file at compile time and produces a `[N]UInt8` vector
// constant whose bytes are exactly the file's contents. The path is the
// raw string from the source; resolution against a base directory and
// (eventually) a content-hashed manifest is the driver's responsibility.
struct EmbedExpr : Expr {
    std::string path;
    EmbedExpr() : Expr(NodeKind::EmbedExpr) {}
};
// `"hello \(name), age \(age + 1)"` — §4 string interpolation. The
// lexer splits the source into alternating literal fragments and
// splice expressions; this node carries them in source order. A
// segment with `expr == nullptr` is a literal fragment whose
// (still-raw, backslash-escapes-not-yet-processed) bytes live in
// `literal`; a segment with `expr != nullptr` is a splice that
// gets rendered via the `Display` protocol (§4) at runtime.
struct InterpStringExpr : Expr {
    struct Segment {
        std::string literal;  // raw bytes, valid when expr == nullptr
        ExprPtr expr;         // a splice, valid when literal.empty()
    };
    std::vector<Segment> segments;
    InterpStringExpr() : Expr(NodeKind::InterpStringExpr) {}
};
struct QuoteExpr : Expr {
    ExprPtr inner;
    QuoteExpr() : Expr(NodeKind::QuoteExpr) {}
};
struct AwaitExpr : Expr {
    ExprPtr inner;
    AwaitExpr() : Expr(NodeKind::AwaitExpr) {}
};
struct SpawnExpr : Expr {
    ExprPtr inner;
    SpawnExpr() : Expr(NodeKind::SpawnExpr) {}
};
struct CopyExpr : Expr {
    ExprPtr inner;
    CopyExpr() : Expr(NodeKind::CopyExpr) {}
};
struct ThrowExpr : Expr {
    ExprPtr inner;
    ThrowExpr() : Expr(NodeKind::ThrowExpr) {}
};

// §9 `do { body } catch (NAME: E) { handler }` — inline error handling.
// The body is checked with `E` as its enclosing throws-context, so any
// `try f()` inside propagates a Result<T, E> via std::unexpected. If the
// body completes without error, the expression yields the body's value;
// otherwise control transfers to the catch arm with NAME bound to E.
// Both arms must produce values of the same success type T. V0.5
// requires the explicit `(NAME: E)` annotation; bare `catch NAME` (with
// inferred E from the body) is a follow-on.
struct DoCatchExpr : Expr {
    ExprPtr do_body;
    std::string error_name;
    TypePtr error_type;
    ExprPtr catch_body;
    DoCatchExpr() : Expr(NodeKind::DoCatchExpr) {}
};

struct TryExpr : Expr {
    enum class Form { Propagating, Optional, Forced };  // try, try?, try!
    Form form;
    ExprPtr inner;
    TryExpr() : Expr(NodeKind::TryExpr) {}
};

struct VectorLitExpr : Expr {
    std::vector<ExprPtr> elements;
    VectorLitExpr() : Expr(NodeKind::VectorLitExpr) {}
};

// §6 `(e1, e2, …)` — a tuple literal expression. At least two
// elements; a single `(e)` parses as ParenExpr to avoid confusion
// with grouping. Lowers to `std::tuple<T1, T2, …>(e1, e2, …)`.
struct TupleLitExpr : Expr {
    std::vector<ExprPtr> elements;
    TupleLitExpr() : Expr(NodeKind::TupleLitExpr) {}
};

struct ParenExpr : Expr {
    ExprPtr inner;
    ParenExpr() : Expr(NodeKind::ParenExpr) {}
};

// ------------------------------------------------------------------ stmts

struct LetStmt : Stmt {
    PatternPtr pattern;
    TypePtr type;  // optional
    ExprPtr value;
    LetStmt() : Stmt(NodeKind::LetStmt) {}
};

struct VarStmt : Stmt {
    PatternPtr pattern;
    TypePtr type;
    ExprPtr value;
    VarStmt() : Stmt(NodeKind::VarStmt) {}
};

struct ExprStmt : Stmt {
    ExprPtr expr;
    ExprStmt() : Stmt(NodeKind::ExprStmt) {}
};

struct ReturnStmt : Stmt {
    ExprPtr value;  // optional
    ReturnStmt() : Stmt(NodeKind::ReturnStmt) {}
};

struct BreakStmt : Stmt {
    std::string label;
    BreakStmt() : Stmt(NodeKind::BreakStmt) {}
};
struct ContinueStmt : Stmt {
    std::string label;
    ContinueStmt() : Stmt(NodeKind::ContinueStmt) {}
};

struct ForStmt : Stmt {
    std::string label;
    PatternPtr pattern;
    ExprPtr iter;
    ExprPtr body;
    ForStmt() : Stmt(NodeKind::ForStmt) {}
};

struct WhileStmt : Stmt {
    std::string label;
    ExprPtr cond;
    ExprPtr body;
    WhileStmt() : Stmt(NodeKind::WhileStmt) {}
};

struct WithBinding {
    // §17.4 admits three w-bind shapes:
    //   `with TYPE { ... }`         — marker capability (no value, no name)
    //   `with TYPE = EXPR { ... }`  — capability satisfaction with a value
    //   `with name = EXPR { ... }`  — value binding (no capability)
    // The parser disambiguates the third shape by the leading
    // identifier's first letter: lowercase → name binding; uppercase
    // → capability type. When `name` is non-empty, `cap_type` stays
    // null and only the value's lexical scope matters.
    std::string name;
    TypePtr cap_type;
    ExprPtr value;
};

struct WithStmt : Stmt {
    std::vector<WithBinding> bindings;
    ExprPtr body;
    WithStmt() : Stmt(NodeKind::WithStmt) {}
};

struct AssignStmt : Stmt {
    AssignOp op;
    ExprPtr target;
    ExprPtr value;
    AssignStmt() : Stmt(NodeKind::AssignStmt) {}
};

// ------------------------------------------------------------------ patterns

struct WildcardPat : Pattern {
    WildcardPat() : Pattern(NodeKind::WildcardPat) {}
};
struct LiteralPat : Pattern {
    ExprPtr literal;
    LiteralPat() : Pattern(NodeKind::LiteralPat) {}
};
struct BindPat : Pattern {
    std::string name;
    BindPat() : Pattern(NodeKind::BindPat) {}
};
struct IdentPat : Pattern {
    std::string name;
    IdentPat() : Pattern(NodeKind::IdentPat) {}
};

struct EnumPat : Pattern {
    std::string case_name;
    std::vector<PatternPtr> children;
    EnumPat() : Pattern(NodeKind::EnumPat) {}
};

// §6 tuple destructuring: `let (a, b) = pair` parses as a TuplePat
// whose elements are each their own sub-pattern. v0.5 requires at
// least two elements (a single `(p)` stays an inner pattern).
struct TuplePat : Pattern {
    std::vector<PatternPtr> elements;
    TuplePat() : Pattern(NodeKind::TuplePat) {}
};

// §17.7 range pattern at match-arm position: `case 0 ..< 10:` or
// `case 0 .. 10:`. Bounds must be integer literals (or comptime-
// foldable integer expressions); the scrutinee must be integer.
// `inclusive` mirrors the BinaryOp::Range / RangeLt distinction in
// expression position — `..` is inclusive, `..<` is exclusive.
struct RangePat : Pattern {
    ExprPtr low;
    ExprPtr high;
    bool inclusive = false;
    RangePat() : Pattern(NodeKind::RangePat) {}
};

// §17.7 or-pattern: `case .red | .green:` or `case 1 | 2 | 3:`. v0.5
// rejects alternatives that introduce conflicting bindings — all
// alternatives must either name the same binding(s) of the same
// type, or none at all. Each alt is parsed as an ordinary pattern.
struct OrPat : Pattern {
    std::vector<PatternPtr> alternatives;
    OrPat() : Pattern(NodeKind::OrPat) {}
};

// ------------------------------------------------------------------ decls

struct ModuleDecl : Decl {
    std::vector<std::string> path;
    ModuleDecl() : Decl(NodeKind::Module) {}
};

struct ImportDecl : Decl {
    bool is_c_header = false;
    std::string c_header;           // when is_c_header
    std::vector<std::string> path;  // otherwise
    ImportDecl() : Decl(NodeKind::Import) {}
};

struct FuncDecl : Decl {
    std::vector<Attribute> attributes;
    Visibility visibility = Visibility::Internal;
    bool is_async = false;
    // §12.4 / §12.3: `comptime func` runs at compile time. The comptime
    // folder is allowed to call it from a const initializer; ordinary
    // (non-comptime) callers may also call it (it then just behaves like
    // a regular pure function as far as runtime semantics go).
    bool is_comptime = false;
    // Method receivers from §17.2's recv-mode.
    enum class RecvMode { None, Inout, Sink };
    RecvMode recv_mode = RecvMode::None;
    std::string name;
    std::vector<GenericParam> generics;
    std::vector<Param> params;
    Effects effects;
    TypePtr result;  // null = Unit
    ExprPtr body;    // null = declaration only (e.g. protocol member)
    FuncDecl() : Decl(NodeKind::Func) {}
};

struct StructDecl : Decl {
    std::vector<Attribute> attributes;
    Visibility visibility = Visibility::Internal;
    std::string name;
    std::vector<GenericParam> generics;

    struct Field {
        std::vector<Attribute> attributes;
        enum class Kind { Let, Var, Embed };
        Kind kind = Kind::Var;
        std::string name;
        TypePtr type;
        diag::SourceRange range;
    };
    std::vector<Field> fields;
    std::vector<DeclPtr> methods;  // FuncDecl entries
    ExprPtr deinit_body;           // optional
    StructDecl() : Decl(NodeKind::Struct) {}
};

struct EnumDecl : Decl {
    std::vector<Attribute> attributes;
    Visibility visibility = Visibility::Internal;
    std::string name;
    std::vector<GenericParam> generics;

    struct Case {
        std::string name;
        // (name, type) — name empty if positional.
        std::vector<std::pair<std::string, TypePtr>> payload;
        diag::SourceRange range;
    };
    std::vector<Case> cases;
    std::vector<DeclPtr> methods;
    EnumDecl() : Decl(NodeKind::Enum) {}
};

struct ProtocolDecl : Decl {
    std::vector<Attribute> attributes;
    Visibility visibility = Visibility::Internal;
    std::string name;
    std::vector<GenericParam> generics;
    std::vector<TypePtr> refines;  // `protocol Ord: Eq` — listed bounds

    struct AssocType {
        std::string name;
        Bound bound;
    };
    std::vector<AssocType> assoc_types;
    std::vector<DeclPtr> requirements;  // FuncDecls (with optional comptime body)
    ProtocolDecl() : Decl(NodeKind::Protocol) {}
};

struct ExtensionDecl : Decl {
    std::vector<Attribute> attributes;
    Visibility visibility = Visibility::Internal;
    TypePtr target;
    std::vector<TypePtr> conformances;
    std::vector<DeclPtr> members;
    ExtensionDecl() : Decl(NodeKind::Extension) {}
};

struct OpaqueDecl : Decl {
    Visibility visibility = Visibility::Internal;
    std::string name;
    TypePtr underlying;
    OpaqueDecl() : Decl(NodeKind::Opaque) {}
};

struct ConstDecl : Decl {
    Visibility visibility = Visibility::Internal;
    std::string name;
    TypePtr type;  // optional
    ExprPtr value;
    ConstDecl() : Decl(NodeKind::Const) {}
};

struct StaticDecl : Decl {
    Visibility visibility = Visibility::Internal;
    std::string name;
    TypePtr type;
    ExprPtr value;
    StaticDecl() : Decl(NodeKind::Static) {}
};

struct DeriveDecl : Decl {
    std::vector<TypePtr> protocols;
    TypePtr target;
    DeriveDecl() : Decl(NodeKind::Derive) {}
};

// ------------------------------------------------------------------ root

// A whole source file: an optional `module` + zero-or-more imports + decls.
struct CompilationUnit {
    std::unique_ptr<ModuleDecl> module;
    std::vector<std::unique_ptr<ImportDecl>> imports;
    std::vector<DeclPtr> decls;
};

}  // namespace vestra::ast
