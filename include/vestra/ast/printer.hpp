// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#pragma once

#include "vestra/ast/nodes.hpp"

#include <iosfwd>
#include <string>

namespace vestra::ast {

// Pretty-print an AST back to canonical Vestra source. This is the round-trip
// counterpart of the parser and the foundation of `vestra fmt`.
class Printer {
public:
    Printer() = default;

    [[nodiscard]] std::string print(const CompilationUnit& unit);
    void print_to(std::ostream& os, const CompilationUnit& unit);

private:
    void print_decl(std::ostream& os, const Decl& d, int indent);
    void print_stmt(std::ostream& os, const Stmt& s, int indent);
    void print_expr(std::ostream& os, const Expr& e);
    void print_type(std::ostream& os, const Type& t);
    void print_pattern(std::ostream& os, const Pattern& p);

    void print_func(std::ostream& os, const FuncDecl& f, int indent);
    void print_struct(std::ostream& os, const StructDecl& s, int indent);
    void print_enum(std::ostream& os, const EnumDecl& e, int indent);
    void print_protocol(std::ostream& os, const ProtocolDecl& p, int indent);

    void print_block(std::ostream& os, const BlockExpr& b, int indent);

    static void indent_to(std::ostream& os, int indent);
    static const char* visibility_kw(Visibility v);
    static const char* binary_op_text(BinaryOp op);
    static const char* unary_op_text(UnaryOp op);
    static const char* assign_op_text(AssignOp op);
};

}  // namespace vestra::ast
