// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#include "vestra/ast/printer.hpp"

#include <ostream>
#include <sstream>
#include <string>

namespace vestra::ast {

// ----- public ---------------------------------------------------------------

std::string Printer::print(const CompilationUnit& unit) {
    std::ostringstream os;
    print_to(os, unit);
    return os.str();
}

void Printer::print_to(std::ostream& os, const CompilationUnit& unit) {
    if (unit.module) {
        os << "module ";
        for (std::size_t i = 0; i < unit.module->path.size(); ++i) {
            if (i != 0) {
                os << ".";
            }
            os << unit.module->path[i];
        }
        os << "\n\n";
    }
    for (const auto& imp : unit.imports) {
        if (imp->is_c_header) {
            os << "import c \"" << imp->c_header << "\"\n";
        } else {
            os << "import ";
            for (std::size_t i = 0; i < imp->path.size(); ++i) {
                if (i != 0) {
                    os << ".";
                }
                os << imp->path[i];
            }
            os << "\n";
        }
    }
    if (!unit.imports.empty()) {
        os << "\n";
    }
    for (const auto& d : unit.decls) {
        print_decl(os, *d, 0);
        os << "\n";
    }
}

// ----- small helpers --------------------------------------------------------

void Printer::indent_to(std::ostream& os, int indent) {
    for (int i = 0; i < indent; ++i) {
        os << "    ";
    }
}

const char* Printer::visibility_kw(Visibility v) {
    switch (v) {
    case Visibility::Private:
        return "private ";
    case Visibility::Internal:
        return "";  // default; omit
    case Visibility::Package:
        return "package ";
    case Visibility::Public:
        return "public ";
    }
    return "";
}

const char* Printer::binary_op_text(BinaryOp op) {
    switch (op) {
    case BinaryOp::Mul:
        return "*";
    case BinaryOp::Div:
        return "/";
    case BinaryOp::Mod:
        return "%";
    case BinaryOp::Add:
        return "+";
    case BinaryOp::Sub:
        return "-";
    case BinaryOp::Shl:
        return "<<";
    case BinaryOp::Shr:
        return ">>";
    case BinaryOp::BitAnd:
        return "&";
    case BinaryOp::BitXor:
        return "^";
    case BinaryOp::BitOr:
        return "|";
    case BinaryOp::Range:
        return "..";
    case BinaryOp::RangeLt:
        return "..<";
    case BinaryOp::Eq:
        return "==";
    case BinaryOp::Ne:
        return "!=";
    case BinaryOp::Lt:
        return "<";
    case BinaryOp::Le:
        return "<=";
    case BinaryOp::Gt:
        return ">";
    case BinaryOp::Ge:
        return ">=";
    case BinaryOp::And:
        return "&&";
    case BinaryOp::Or:
        return "||";
    case BinaryOp::Coalesce:
        return "??";
    case BinaryOp::WrappingAdd:
        return "&+";
    case BinaryOp::WrappingSub:
        return "&-";
    case BinaryOp::WrappingMul:
        return "&*";
    }
    return "?";
}

const char* Printer::unary_op_text(UnaryOp op) {
    switch (op) {
    case UnaryOp::Neg:
        return "-";
    case UnaryOp::Not:
        return "!";
    case UnaryOp::BitNot:
        return "~";
    case UnaryOp::Unwrap:
        return "!";  // postfix in source; printer renders prefix, accepted
    }
    return "?";
}

const char* Printer::assign_op_text(AssignOp op) {
    switch (op) {
    case AssignOp::Assign:
        return "=";
    case AssignOp::AddAssign:
        return "+=";
    case AssignOp::SubAssign:
        return "-=";
    case AssignOp::MulAssign:
        return "*=";
    case AssignOp::DivAssign:
        return "/=";
    case AssignOp::ModAssign:
        return "%=";
    case AssignOp::BitAndAssign:
        return "&=";
    case AssignOp::BitOrAssign:
        return "|=";
    case AssignOp::BitXorAssign:
        return "^=";
    case AssignOp::ShlAssign:
        return "<<=";
    case AssignOp::ShrAssign:
        return ">>=";
    }
    return "?=";
}

// ----- declarations ---------------------------------------------------------

void Printer::print_decl(std::ostream& os, const Decl& d, int indent) {
    switch (d.kind) {
    case NodeKind::Func:
        print_func(os, static_cast<const FuncDecl&>(d), indent);
        break;
    case NodeKind::Struct:
        print_struct(os, static_cast<const StructDecl&>(d), indent);
        break;
    case NodeKind::Enum:
        print_enum(os, static_cast<const EnumDecl&>(d), indent);
        break;
    case NodeKind::Protocol:
        print_protocol(os, static_cast<const ProtocolDecl&>(d), indent);
        break;

    case NodeKind::Const: {
        const auto& c = static_cast<const ConstDecl&>(d);
        indent_to(os, indent);
        os << visibility_kw(c.visibility) << "const " << c.name;
        if (c.type) {
            os << ": ";
            print_type(os, *c.type);
        }
        os << " = ";
        if (c.value) {
            print_expr(os, *c.value);
        }
        os << "\n";
        break;
    }
    case NodeKind::Static: {
        const auto& s = static_cast<const StaticDecl&>(d);
        indent_to(os, indent);
        os << visibility_kw(s.visibility) << "static " << s.name << ": ";
        print_type(os, *s.type);
        os << " = ";
        print_expr(os, *s.value);
        os << "\n";
        break;
    }
    case NodeKind::Opaque: {
        const auto& o = static_cast<const OpaqueDecl&>(d);
        indent_to(os, indent);
        os << visibility_kw(o.visibility) << "opaque type " << o.name << " = ";
        print_type(os, *o.underlying);
        os << "\n";
        break;
    }
    case NodeKind::Derive: {
        const auto& dv = static_cast<const DeriveDecl&>(d);
        indent_to(os, indent);
        os << "derive(";
        for (std::size_t i = 0; i < dv.protocols.size(); ++i) {
            if (i != 0) {
                os << ", ";
            }
            print_type(os, *dv.protocols[i]);
        }
        os << ") for ";
        print_type(os, *dv.target);
        os << "\n";
        break;
    }
    case NodeKind::Extension: {
        const auto& e = static_cast<const ExtensionDecl&>(d);
        indent_to(os, indent);
        os << "extension ";
        print_type(os, *e.target);
        if (!e.conformances.empty()) {
            os << ": ";
            for (std::size_t i = 0; i < e.conformances.size(); ++i) {
                if (i != 0) {
                    os << ", ";
                }
                print_type(os, *e.conformances[i]);
            }
        }
        os << " {\n";
        for (const auto& m : e.members) {
            print_decl(os, *m, indent + 1);
        }
        indent_to(os, indent);
        os << "}\n";
        break;
    }
    default:
        indent_to(os, indent);
        os << "/* unprinted decl */\n";
        break;
    }
}

void Printer::print_func(std::ostream& os, const FuncDecl& f, int indent) {
    indent_to(os, indent);
    os << visibility_kw(f.visibility);
    if (f.is_comptime) {
        os << "comptime ";
    }
    if (f.is_async) {
        os << "async ";
    }
    if (f.recv_mode == FuncDecl::RecvMode::Inout) {
        os << "inout ";
    }
    if (f.recv_mode == FuncDecl::RecvMode::Sink) {
        os << "sink ";
    }
    os << "func " << f.name;
    if (!f.generics.empty()) {
        os << "[";
        for (std::size_t i = 0; i < f.generics.size(); ++i) {
            if (i != 0) {
                os << ", ";
            }
            const auto& g = f.generics[i];
            if (g.is_const) {
                os << "const " << g.name << ": ";
                if (g.const_type) {
                    print_type(os, *g.const_type);
                }
            } else {
                os << g.name;
                if (!g.bound.protocols.empty()) {
                    os << ": ";
                    for (std::size_t k = 0; k < g.bound.protocols.size(); ++k) {
                        if (k != 0) {
                            os << " & ";
                        }
                        print_type(os, *g.bound.protocols[k]);
                    }
                }
            }
        }
        os << "]";
    }
    os << "(";
    for (std::size_t i = 0; i < f.params.size(); ++i) {
        if (i != 0) {
            os << ", ";
        }
        const auto& p = f.params[i];
        if (p.label_omitted) {
            os << "_ ";
        } else if (!p.label.empty() && p.label != p.name) {
            os << p.label << " ";
        }
        os << p.name << ": ";
        if (p.nonescaping) {
            os << "nonescaping ";
        }
        if (p.mode == ParamMode::Inout) {
            os << "inout ";
        }
        if (p.mode == ParamMode::Sink) {
            os << "sink ";
        }
        if (p.type) {
            print_type(os, *p.type);
        }
    }
    os << ")";
    if (!f.effects.using_caps.empty()) {
        os << " using ";
        for (std::size_t i = 0; i < f.effects.using_caps.size(); ++i) {
            if (i != 0) {
                os << ", ";
            }
            print_type(os, *f.effects.using_caps[i]);
        }
    }
    if (f.effects.throws_type) {
        os << " throws(";
        print_type(os, *f.effects.throws_type);
        os << ")";
    }
    if (f.result) {
        os << " -> ";
        print_type(os, *f.result);
    }
    if (f.body) {
        os << " ";
        print_expr(os, *f.body);
    }
    os << "\n";
}

void Printer::print_struct(std::ostream& os, const StructDecl& s, int indent) {
    indent_to(os, indent);
    os << visibility_kw(s.visibility) << "struct " << s.name << " {\n";
    for (const auto& f : s.fields) {
        indent_to(os, indent + 1);
        switch (f.kind) {
        case StructDecl::Field::Kind::Let:
            os << "let ";
            break;
        case StructDecl::Field::Kind::Var:
            os << "var ";
            break;
        case StructDecl::Field::Kind::Embed:
            os << "embed ";
            break;
        }
        os << f.name << ": ";
        if (f.type) {
            print_type(os, *f.type);
        }
        os << "\n";
    }
    for (const auto& m : s.methods) {
        print_decl(os, *m, indent + 1);
    }
    if (s.deinit_body) {
        indent_to(os, indent + 1);
        os << "deinit ";
        print_expr(os, *s.deinit_body);
        os << "\n";
    }
    indent_to(os, indent);
    os << "}\n";
}

void Printer::print_enum(std::ostream& os, const EnumDecl& e, int indent) {
    indent_to(os, indent);
    os << visibility_kw(e.visibility) << "enum " << e.name << " {\n";
    for (const auto& c : e.cases) {
        indent_to(os, indent + 1);
        os << "case " << c.name;
        if (!c.payload.empty()) {
            os << "(";
            for (std::size_t i = 0; i < c.payload.size(); ++i) {
                if (i != 0) {
                    os << ", ";
                }
                if (!c.payload[i].first.empty()) {
                    os << c.payload[i].first << ": ";
                }
                if (c.payload[i].second) {
                    print_type(os, *c.payload[i].second);
                }
            }
            os << ")";
        }
        os << "\n";
    }
    for (const auto& m : e.methods) {
        print_decl(os, *m, indent + 1);
    }
    indent_to(os, indent);
    os << "}\n";
}

void Printer::print_protocol(std::ostream& os, const ProtocolDecl& p, int indent) {
    indent_to(os, indent);
    os << visibility_kw(p.visibility) << "protocol " << p.name;
    if (!p.refines.empty()) {
        os << ": ";
        for (std::size_t i = 0; i < p.refines.size(); ++i) {
            if (i != 0) {
                os << ", ";
            }
            print_type(os, *p.refines[i]);
        }
    }
    os << " {\n";
    for (const auto& a : p.assoc_types) {
        indent_to(os, indent + 1);
        os << "type " << a.name << "\n";
    }
    for (const auto& r : p.requirements) {
        print_decl(os, *r, indent + 1);
    }
    indent_to(os, indent);
    os << "}\n";
}

// ----- types ----------------------------------------------------------------

void Printer::print_type(std::ostream& os, const Type& t) {
    switch (t.kind) {
    case NodeKind::NamedType: {
        const auto& n = static_cast<const NamedType&>(t);
        for (std::size_t i = 0; i < n.path.size(); ++i) {
            if (i != 0) {
                os << ".";
            }
            os << n.path[i];
        }
        if (n.has_generics) {
            os << "[";
            bool first = true;
            for (const auto& ta : n.type_args) {
                if (!first) {
                    os << ", ";
                }
                first = false;
                print_type(os, *ta);
            }
            for (auto ci : n.const_args) {
                if (!first) {
                    os << ", ";
                }
                first = false;
                os << ci;
            }
            for (const auto& [name, ty] : n.assoc_bindings) {
                if (!first) {
                    os << ", ";
                }
                first = false;
                os << name << " = ";
                if (ty) {
                    print_type(os, *ty);
                }
            }
            os << "]";
        }
        break;
    }
    case NodeKind::VectorType: {
        const auto& v = static_cast<const VectorType&>(t);
        os << "[";
        if (!v.length_ident.empty()) {
            os << v.length_ident;
        } else {
            os << v.length;
        }
        os << "]";
        if (v.element) {
            print_type(os, *v.element);
        }
        break;
    }
    case NodeKind::OptionalType:
        print_type(os, *static_cast<const OptionalType&>(t).inner);
        os << "?";
        break;
    case NodeKind::SomeType:
        os << "some ";
        print_type(os, *static_cast<const SomeType&>(t).inner);
        break;
    case NodeKind::DynType:
        os << "dyn ";
        print_type(os, *static_cast<const DynType&>(t).inner);
        break;
    case NodeKind::TupleType: {
        const auto& tup = static_cast<const TupleType&>(t);
        os << "(";
        for (std::size_t i = 0; i < tup.elements.size(); ++i) {
            if (i != 0) {
                os << ", ";
            }
            print_type(os, *tup.elements[i]);
        }
        os << ")";
        break;
    }
    case NodeKind::FunctionType: {
        const auto& f = static_cast<const FunctionType&>(t);
        os << "(";
        for (std::size_t i = 0; i < f.params.size(); ++i) {
            if (i != 0) {
                os << ", ";
            }
            print_type(os, *f.params[i]);
        }
        os << ")";
        if (!f.effects.using_caps.empty()) {
            os << " using ";
            for (std::size_t i = 0; i < f.effects.using_caps.size(); ++i) {
                if (i != 0) {
                    os << ", ";
                }
                print_type(os, *f.effects.using_caps[i]);
            }
        }
        if (f.effects.throws_type) {
            os << " throws(";
            print_type(os, *f.effects.throws_type);
            os << ")";
        }
        if (f.result) {
            os << " -> ";
            print_type(os, *f.result);
        }
        break;
    }
    default:
        os << "/*?type*/";
        break;
    }
}

// ----- exprs / stmts / patterns ---------------------------------------------

void Printer::print_block(std::ostream& os, const BlockExpr& b, int indent) {
    os << "{\n";
    for (const auto& s : b.stmts) {
        print_stmt(os, *s, indent + 1);
    }
    indent_to(os, indent);
    os << "}";
}

void Printer::print_stmt(std::ostream& os, const Stmt& s, int indent) {
    indent_to(os, indent);
    switch (s.kind) {
    case NodeKind::LetStmt: {
        const auto& l = static_cast<const LetStmt&>(s);
        os << "let ";
        if (l.pattern) {
            print_pattern(os, *l.pattern);
        }
        if (l.type) {
            os << ": ";
            print_type(os, *l.type);
        }
        os << " = ";
        if (l.value) {
            print_expr(os, *l.value);
        }
        os << "\n";
        break;
    }
    case NodeKind::VarStmt: {
        const auto& v = static_cast<const VarStmt&>(s);
        os << "var ";
        if (v.pattern) {
            print_pattern(os, *v.pattern);
        }
        if (v.type) {
            os << ": ";
            print_type(os, *v.type);
        }
        os << " = ";
        if (v.value) {
            print_expr(os, *v.value);
        }
        os << "\n";
        break;
    }
    case NodeKind::ExprStmt:
        print_expr(os, *static_cast<const ExprStmt&>(s).expr);
        os << "\n";
        break;
    case NodeKind::ReturnStmt: {
        const auto& r = static_cast<const ReturnStmt&>(s);
        os << "return";
        if (r.value) {
            os << " ";
            print_expr(os, *r.value);
        }
        os << "\n";
        break;
    }
    case NodeKind::BreakStmt:
        os << "break";
        if (!static_cast<const BreakStmt&>(s).label.empty()) {
            os << " " << static_cast<const BreakStmt&>(s).label;
        }
        os << "\n";
        break;
    case NodeKind::ContinueStmt:
        os << "continue";
        if (!static_cast<const ContinueStmt&>(s).label.empty()) {
            os << " " << static_cast<const ContinueStmt&>(s).label;
        }
        os << "\n";
        break;
    case NodeKind::AssignStmt: {
        const auto& a = static_cast<const AssignStmt&>(s);
        print_expr(os, *a.target);
        os << " " << assign_op_text(a.op) << " ";
        print_expr(os, *a.value);
        os << "\n";
        break;
    }
    case NodeKind::WhileStmt: {
        const auto& w = static_cast<const WhileStmt&>(s);
        os << "while ";
        print_expr(os, *w.cond);
        os << " ";
        print_expr(os, *w.body);
        os << "\n";
        break;
    }
    case NodeKind::ForStmt: {
        const auto& f = static_cast<const ForStmt&>(s);
        os << "for ";
        if (f.pattern) {
            print_pattern(os, *f.pattern);
        }
        os << " in ";
        print_expr(os, *f.iter);
        os << " ";
        print_expr(os, *f.body);
        os << "\n";
        break;
    }
    default:
        os << "/*?stmt*/\n";
        break;
    }
}

void Printer::print_expr(std::ostream& os, const Expr& e) {
    switch (e.kind) {
    case NodeKind::IntLit:
        os << static_cast<const IntLit&>(e).text;
        break;
    case NodeKind::FloatLit:
        os << static_cast<const FloatLit&>(e).text;
        break;
    case NodeKind::StringLit:
        os << "\"" << static_cast<const StringLit&>(e).text << "\"";
        break;
    case NodeKind::ByteStringLit:
        os << "b\"" << static_cast<const ByteStringLit&>(e).text << "\"";
        break;
    case NodeKind::CharLit:
        os << "'" << static_cast<const CharLit&>(e).text << "'";
        break;
    case NodeKind::BoolLit:
        os << (static_cast<const BoolLit&>(e).value ? "true" : "false");
        break;
    case NodeKind::IdentExpr:
        os << static_cast<const IdentExpr&>(e).name;
        break;
    case NodeKind::SelfExpr:
        os << "self";
        break;
    case NodeKind::LeadingDotExpr:
        os << "." << static_cast<const LeadingDotExpr&>(e).name;
        break;
    case NodeKind::PathExpr: {
        const auto& p = static_cast<const PathExpr&>(e);
        for (std::size_t i = 0; i < p.segments.size(); ++i) {
            if (i != 0) {
                os << ".";
            }
            os << p.segments[i];
        }
        break;
    }
    case NodeKind::ParenExpr:
        os << "(";
        print_expr(os, *static_cast<const ParenExpr&>(e).inner);
        os << ")";
        break;
    case NodeKind::UnaryExpr: {
        const auto& u = static_cast<const UnaryExpr&>(e);
        os << unary_op_text(u.op);
        print_expr(os, *u.operand);
        break;
    }
    case NodeKind::BinaryExpr: {
        const auto& b = static_cast<const BinaryExpr&>(e);
        print_expr(os, *b.lhs);
        os << " " << binary_op_text(b.op) << " ";
        print_expr(os, *b.rhs);
        break;
    }
    case NodeKind::CallExpr: {
        const auto& c = static_cast<const CallExpr&>(e);
        print_expr(os, *c.callee);
        os << "(";
        for (std::size_t i = 0; i < c.args.size(); ++i) {
            if (i != 0) {
                os << ", ";
            }
            if (!c.args[i].label.empty()) {
                os << c.args[i].label << ": ";
            }
            if (c.args[i].is_inout) {
                os << "&";
            }
            print_expr(os, *c.args[i].value);
        }
        os << ")";
        break;
    }
    case NodeKind::MemberExpr: {
        const auto& m = static_cast<const MemberExpr&>(e);
        print_expr(os, *m.base);
        os << "." << m.member;
        break;
    }
    case NodeKind::IndexExpr: {
        const auto& ix = static_cast<const IndexExpr&>(e);
        print_expr(os, *ix.base);
        os << "[";
        for (std::size_t i = 0; i < ix.indices.size(); ++i) {
            if (i != 0) {
                os << ", ";
            }
            print_expr(os, *ix.indices[i]);
        }
        os << "]";
        break;
    }
    case NodeKind::IfExpr: {
        const auto& i = static_cast<const IfExpr&>(e);
        os << "if ";
        if (!i.let_name.empty()) {
            os << "let " << i.let_name << " = ";
            if (i.let_init) {
                print_expr(os, *i.let_init);
            }
        } else if (i.cond) {
            print_expr(os, *i.cond);
        }
        os << " ";
        print_expr(os, *i.then_branch);
        if (i.else_branch) {
            os << " else ";
            print_expr(os, *i.else_branch);
        }
        break;
    }
    case NodeKind::BlockExpr:
        print_block(os, static_cast<const BlockExpr&>(e), 0);
        break;
    case NodeKind::CopyExpr:
        os << "copy ";
        print_expr(os, *static_cast<const CopyExpr&>(e).inner);
        break;
    case NodeKind::AwaitExpr:
        os << "await ";
        print_expr(os, *static_cast<const AwaitExpr&>(e).inner);
        break;
    case NodeKind::SpawnExpr:
        os << "spawn ";
        print_expr(os, *static_cast<const SpawnExpr&>(e).inner);
        break;
    case NodeKind::TryExpr: {
        const auto& t = static_cast<const TryExpr&>(e);
        switch (t.form) {
        case TryExpr::Form::Propagating:
            os << "try ";
            break;
        case TryExpr::Form::Optional:
            os << "try? ";
            break;
        case TryExpr::Form::Forced:
            os << "try! ";
            break;
        }
        print_expr(os, *t.inner);
        break;
    }
    case NodeKind::AsExpr: {
        const auto& a = static_cast<const AsExpr&>(e);
        print_expr(os, *a.value);
        os << (a.is_optional ? " as? " : " as ");
        print_type(os, *a.target);
        break;
    }
    default:
        os << "/*?expr*/";
        break;
    }
}

void Printer::print_pattern(std::ostream& os, const Pattern& p) {
    switch (p.kind) {
    case NodeKind::WildcardPat:
        os << "_";
        break;
    case NodeKind::LiteralPat:
        if (auto* lp = static_cast<const LiteralPat&>(p).literal.get()) {
            print_expr(os, *lp);
        }
        break;
    case NodeKind::BindPat:
        os << "let " << static_cast<const BindPat&>(p).name;
        break;
    case NodeKind::IdentPat:
        os << static_cast<const IdentPat&>(p).name;
        break;
    case NodeKind::EnumPat: {
        const auto& ep = static_cast<const EnumPat&>(p);
        os << "." << ep.case_name;
        if (!ep.children.empty()) {
            os << "(";
            for (std::size_t i = 0; i < ep.children.size(); ++i) {
                if (i != 0) {
                    os << ", ";
                }
                print_pattern(os, *ep.children[i]);
            }
            os << ")";
        }
        break;
    }
    default:
        os << "/*?pat*/";
        break;
    }
}

}  // namespace vestra::ast
