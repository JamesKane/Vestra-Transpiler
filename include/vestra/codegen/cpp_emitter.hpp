#pragma once

#include "vestra/ast/nodes.hpp"
#include "vestra/diag/diagnostic.hpp"
#include "vestra/sema/resolver.hpp"

#include <iosfwd>
#include <string>

namespace vestra::codegen {

// What the emitter writes for one Vestra source file.
struct EmittedUnit {
    std::string header;  // .hpp — types, function declarations
    std::string source;  // .cpp — function bodies, statics
};

// Translate a parsed Vestra compilation unit to C++23.
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

    void emit_block(std::ostream& os, const ast::BlockExpr& b, int indent);
    void emit_stmt(std::ostream& os, const ast::Stmt& s, int indent);
    void emit_expr(std::ostream& os, const ast::Expr& e);
    void emit_match(std::ostream& os, const ast::MatchExpr& m);
    void emit_type(std::ostream& os, const ast::Type& t);

    void unsupported(std::ostream& os, std::string_view what, diag::SourceRange r);

    static const char* binop_text(ast::BinaryOp op);
    static const char* unop_text(ast::UnaryOp op);

    diag::DiagnosticReporter* reporter_;
    const sema::Resolution* resolution_ = nullptr;
};

}  // namespace vestra::codegen
