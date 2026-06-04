// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

// §12.4 AST deep-clone — see clone.hpp. Mechanical recursive copy preserving
// source ranges; covers the node kinds a v0.5 macro `quote { … }` template
// uses, with an `<unsupported>` placeholder for anything else.

#include "vestra/ast/clone.hpp"

#include <memory>
#include <utility>

namespace vestra::ast {

namespace {

// Null-safe child-clone wrappers.
ExprPtr cl(const ExprPtr& e) {
    return e ? clone(*e) : nullptr;
}
StmtPtr cls(const StmtPtr& s) {
    return s ? clone(*s) : nullptr;
}
DeclPtr cld(const DeclPtr& d) {
    return d ? clone(*d) : nullptr;
}
TypePtr clt(const TypePtr& t) {
    return t ? clone(*t) : nullptr;
}
PatternPtr clp(const PatternPtr& p) {
    return p ? clone(*p) : nullptr;
}

template <class T>
std::unique_ptr<T> with_range(std::unique_ptr<T> n, const Node& src) {
    n->range = src.range;
    return n;
}

std::vector<TypePtr> clone_types(const std::vector<TypePtr>& in) {
    std::vector<TypePtr> out;
    out.reserve(in.size());
    for (const auto& t : in) {
        out.push_back(clt(t));
    }
    return out;
}

Attribute clone_attr(const Attribute& a) {
    Attribute c;
    c.name = a.name;
    c.predicate = cl(a.predicate);
    for (const auto& e : a.extra_args) {
        c.extra_args.push_back(cl(e));
    }
    c.range = a.range;
    return c;
}

std::vector<Attribute> clone_attrs(const std::vector<Attribute>& in) {
    std::vector<Attribute> out;
    out.reserve(in.size());
    for (const auto& a : in) {
        out.push_back(clone_attr(a));
    }
    return out;
}

Bound clone_bound(const Bound& b) {
    Bound c;
    c.protocols = clone_types(b.protocols);
    return c;
}

Effects clone_effects(const Effects& e) {
    Effects c;
    c.using_caps = clone_types(e.using_caps);
    c.throws_type = clt(e.throws_type);
    return c;
}

GenericParam clone_generic(const GenericParam& g) {
    GenericParam c;
    c.name = g.name;
    c.is_const = g.is_const;
    c.const_type = clt(g.const_type);
    c.bound = clone_bound(g.bound);
    c.range = g.range;
    return c;
}

std::vector<GenericParam> clone_generics(const std::vector<GenericParam>& in) {
    std::vector<GenericParam> out;
    out.reserve(in.size());
    for (const auto& g : in) {
        out.push_back(clone_generic(g));
    }
    return out;
}

Param clone_param(const Param& p) {
    Param c;
    c.label = p.label;
    c.label_omitted = p.label_omitted;
    c.name = p.name;
    c.nonescaping = p.nonescaping;
    c.mode = p.mode;
    c.type = clt(p.type);
    c.default_value = cl(p.default_value);
    c.pattern = clp(p.pattern);
    c.range = p.range;
    return c;
}

std::vector<Param> clone_params(const std::vector<Param>& in) {
    std::vector<Param> out;
    out.reserve(in.size());
    for (const auto& p : in) {
        out.push_back(clone_param(p));
    }
    return out;
}

ExprPtr unsupported_expr(const Node& src) {
    auto n = std::make_unique<IdentExpr>();
    n->name = "<unsupported-in-quote>";
    n->range = src.range;
    return n;
}

}  // namespace

ExprPtr clone(const Expr& e) {
    switch (e.kind) {
    case NodeKind::IntLit: {
        auto n = std::make_unique<IntLit>();
        n->text = static_cast<const IntLit&>(e).text;
        return with_range(std::move(n), e);
    }
    case NodeKind::FloatLit: {
        auto n = std::make_unique<FloatLit>();
        n->text = static_cast<const FloatLit&>(e).text;
        return with_range(std::move(n), e);
    }
    case NodeKind::StringLit: {
        auto n = std::make_unique<StringLit>();
        n->text = static_cast<const StringLit&>(e).text;
        return with_range(std::move(n), e);
    }
    case NodeKind::ByteStringLit: {
        auto n = std::make_unique<ByteStringLit>();
        n->text = static_cast<const ByteStringLit&>(e).text;
        return with_range(std::move(n), e);
    }
    case NodeKind::CharLit: {
        auto n = std::make_unique<CharLit>();
        n->text = static_cast<const CharLit&>(e).text;
        return with_range(std::move(n), e);
    }
    case NodeKind::BoolLit: {
        auto n = std::make_unique<BoolLit>();
        n->value = static_cast<const BoolLit&>(e).value;
        return with_range(std::move(n), e);
    }
    case NodeKind::NilLit:
        return with_range(std::make_unique<NilLit>(), e);
    case NodeKind::SelfExpr:
        return with_range(std::make_unique<SelfExpr>(), e);
    case NodeKind::IdentExpr: {
        auto n = std::make_unique<IdentExpr>();
        n->name = static_cast<const IdentExpr&>(e).name;
        return with_range(std::move(n), e);
    }
    case NodeKind::LeadingDotExpr: {
        auto n = std::make_unique<LeadingDotExpr>();
        n->name = static_cast<const LeadingDotExpr&>(e).name;
        return with_range(std::move(n), e);
    }
    case NodeKind::PathExpr: {
        auto n = std::make_unique<PathExpr>();
        n->segments = static_cast<const PathExpr&>(e).segments;
        return with_range(std::move(n), e);
    }
    case NodeKind::ParenExpr: {
        auto n = std::make_unique<ParenExpr>();
        n->inner = cl(static_cast<const ParenExpr&>(e).inner);
        return with_range(std::move(n), e);
    }
    case NodeKind::UnaryExpr: {
        const auto& u = static_cast<const UnaryExpr&>(e);
        auto n = std::make_unique<UnaryExpr>();
        n->op = u.op;
        n->operand = cl(u.operand);
        return with_range(std::move(n), e);
    }
    case NodeKind::BinaryExpr: {
        const auto& b = static_cast<const BinaryExpr&>(e);
        auto n = std::make_unique<BinaryExpr>();
        n->op = b.op;
        n->lhs = cl(b.lhs);
        n->rhs = cl(b.rhs);
        return with_range(std::move(n), e);
    }
    case NodeKind::MemberExpr: {
        const auto& m = static_cast<const MemberExpr&>(e);
        auto n = std::make_unique<MemberExpr>();
        n->base = cl(m.base);
        n->member = m.member;
        n->member_splice = cl(m.member_splice);
        n->is_optional_chain = m.is_optional_chain;
        return with_range(std::move(n), e);
    }
    case NodeKind::IndexExpr: {
        const auto& ix = static_cast<const IndexExpr&>(e);
        auto n = std::make_unique<IndexExpr>();
        n->base = cl(ix.base);
        for (const auto& i : ix.indices) {
            n->indices.push_back(cl(i));
        }
        n->type_args = clone_types(ix.type_args);
        return with_range(std::move(n), e);
    }
    case NodeKind::CallExpr: {
        const auto& c = static_cast<const CallExpr&>(e);
        auto n = std::make_unique<CallExpr>();
        n->callee = cl(c.callee);
        n->type_args = clone_types(c.type_args);
        for (const auto& a : c.args) {
            CallExpr::Arg na;
            na.label = a.label;
            na.is_inout = a.is_inout;
            na.value = cl(a.value);
            n->args.push_back(std::move(na));
        }
        return with_range(std::move(n), e);
    }
    case NodeKind::TupleLitExpr: {
        const auto& tl = static_cast<const TupleLitExpr&>(e);
        auto n = std::make_unique<TupleLitExpr>();
        for (const auto& el : tl.elements) {
            n->elements.push_back(cl(el));
        }
        return with_range(std::move(n), e);
    }
    case NodeKind::VectorLitExpr: {
        const auto& vl = static_cast<const VectorLitExpr&>(e);
        auto n = std::make_unique<VectorLitExpr>();
        for (const auto& el : vl.elements) {
            n->elements.push_back(cl(el));
        }
        return with_range(std::move(n), e);
    }
    case NodeKind::IfExpr: {
        const auto& i = static_cast<const IfExpr&>(e);
        auto n = std::make_unique<IfExpr>();
        n->let_name = i.let_name;
        n->let_init = cl(i.let_init);
        n->cond = cl(i.cond);
        n->then_branch = cl(i.then_branch);
        n->else_branch = cl(i.else_branch);
        return with_range(std::move(n), e);
    }
    case NodeKind::BlockExpr: {
        const auto& bl = static_cast<const BlockExpr&>(e);
        auto n = std::make_unique<BlockExpr>();
        for (const auto& s : bl.stmts) {
            n->stmts.push_back(cls(s));
        }
        return with_range(std::move(n), e);
    }
    case NodeKind::CopyExpr: {
        auto n = std::make_unique<CopyExpr>();
        n->inner = cl(static_cast<const CopyExpr&>(e).inner);
        return with_range(std::move(n), e);
    }
    case NodeKind::AwaitExpr: {
        auto n = std::make_unique<AwaitExpr>();
        n->inner = cl(static_cast<const AwaitExpr&>(e).inner);
        return with_range(std::move(n), e);
    }
    case NodeKind::SpawnExpr: {
        auto n = std::make_unique<SpawnExpr>();
        n->inner = cl(static_cast<const SpawnExpr&>(e).inner);
        return with_range(std::move(n), e);
    }
    case NodeKind::ThrowExpr: {
        auto n = std::make_unique<ThrowExpr>();
        n->inner = cl(static_cast<const ThrowExpr&>(e).inner);
        return with_range(std::move(n), e);
    }
    case NodeKind::TryExpr: {
        const auto& tx = static_cast<const TryExpr&>(e);
        auto n = std::make_unique<TryExpr>();
        n->form = tx.form;
        n->inner = cl(tx.inner);
        return with_range(std::move(n), e);
    }
    case NodeKind::QuoteExpr: {
        auto n = std::make_unique<QuoteExpr>();
        n->inner = cl(static_cast<const QuoteExpr&>(e).inner);
        return with_range(std::move(n), e);
    }
    case NodeKind::SpliceExpr: {
        auto n = std::make_unique<SpliceExpr>();
        n->inner = cl(static_cast<const SpliceExpr&>(e).inner);
        return with_range(std::move(n), e);
    }
    default:
        return unsupported_expr(e);
    }
}

StmtPtr clone(const Stmt& s) {
    switch (s.kind) {
    case NodeKind::LetStmt: {
        const auto& l = static_cast<const LetStmt&>(s);
        auto n = std::make_unique<LetStmt>();
        n->pattern = clp(l.pattern);
        n->type = clt(l.type);
        n->value = cl(l.value);
        return with_range(std::move(n), s);
    }
    case NodeKind::VarStmt: {
        const auto& v = static_cast<const VarStmt&>(s);
        auto n = std::make_unique<VarStmt>();
        n->pattern = clp(v.pattern);
        n->type = clt(v.type);
        n->value = cl(v.value);
        return with_range(std::move(n), s);
    }
    case NodeKind::ExprStmt: {
        auto n = std::make_unique<ExprStmt>();
        n->expr = cl(static_cast<const ExprStmt&>(s).expr);
        return with_range(std::move(n), s);
    }
    case NodeKind::ReturnStmt: {
        auto n = std::make_unique<ReturnStmt>();
        n->value = cl(static_cast<const ReturnStmt&>(s).value);
        return with_range(std::move(n), s);
    }
    case NodeKind::AssignStmt: {
        const auto& a = static_cast<const AssignStmt&>(s);
        auto n = std::make_unique<AssignStmt>();
        n->op = a.op;
        n->target = cl(a.target);
        n->value = cl(a.value);
        return with_range(std::move(n), s);
    }
    case NodeKind::ForStmt: {
        const auto& f = static_cast<const ForStmt&>(s);
        auto n = std::make_unique<ForStmt>();
        n->label = f.label;
        n->pattern = clp(f.pattern);
        n->iter = cl(f.iter);
        n->body = cl(f.body);
        return with_range(std::move(n), s);
    }
    case NodeKind::WhileStmt: {
        const auto& w = static_cast<const WhileStmt&>(s);
        auto n = std::make_unique<WhileStmt>();
        n->label = w.label;
        n->cond = cl(w.cond);
        n->body = cl(w.body);
        return with_range(std::move(n), s);
    }
    case NodeKind::BreakStmt: {
        auto n = std::make_unique<BreakStmt>();
        n->label = static_cast<const BreakStmt&>(s).label;
        return with_range(std::move(n), s);
    }
    case NodeKind::ContinueStmt: {
        auto n = std::make_unique<ContinueStmt>();
        n->label = static_cast<const ContinueStmt&>(s).label;
        return with_range(std::move(n), s);
    }
    default: {
        // Best-effort: wrap a placeholder so the tree stays well-formed.
        auto n = std::make_unique<ExprStmt>();
        n->expr = unsupported_expr(s);
        return with_range(std::move(n), s);
    }
    }
}

TypePtr clone(const Type& t) {
    switch (t.kind) {
    case NodeKind::NamedType: {
        const auto& nt = static_cast<const NamedType&>(t);
        auto n = std::make_unique<NamedType>();
        n->path = nt.path;
        n->type_args = clone_types(nt.type_args);
        for (const auto& [name, ty] : nt.assoc_bindings) {
            n->assoc_bindings.emplace_back(name, clt(ty));
        }
        n->const_args = nt.const_args;
        n->has_generics = nt.has_generics;
        return with_range(std::move(n), t);
    }
    case NodeKind::OptionalType: {
        auto n = std::make_unique<OptionalType>();
        n->inner = clt(static_cast<const OptionalType&>(t).inner);
        return with_range(std::move(n), t);
    }
    case NodeKind::VectorType: {
        const auto& vt = static_cast<const VectorType&>(t);
        auto n = std::make_unique<VectorType>();
        n->length = vt.length;
        n->length_ident = vt.length_ident;
        n->element = clt(vt.element);
        return with_range(std::move(n), t);
    }
    case NodeKind::FunctionType: {
        const auto& ft = static_cast<const FunctionType&>(t);
        auto n = std::make_unique<FunctionType>();
        n->params = clone_types(ft.params);
        n->effects = clone_effects(ft.effects);
        n->result = clt(ft.result);
        return with_range(std::move(n), t);
    }
    case NodeKind::TupleType: {
        auto n = std::make_unique<TupleType>();
        n->elements = clone_types(static_cast<const TupleType&>(t).elements);
        return with_range(std::move(n), t);
    }
    case NodeKind::SomeType: {
        auto n = std::make_unique<SomeType>();
        n->inner = clt(static_cast<const SomeType&>(t).inner);
        return with_range(std::move(n), t);
    }
    case NodeKind::DynType: {
        auto n = std::make_unique<DynType>();
        n->inner = clt(static_cast<const DynType&>(t).inner);
        return with_range(std::move(n), t);
    }
    case NodeKind::SpliceType: {
        auto n = std::make_unique<SpliceType>();
        n->splice = cl(static_cast<const SpliceType&>(t).splice);
        return with_range(std::move(n), t);
    }
    case NodeKind::InterruptType: {
        auto n = std::make_unique<InterruptType>();
        n->trap_frame = clt(static_cast<const InterruptType&>(t).trap_frame);
        return with_range(std::move(n), t);
    }
    default: {
        auto n = std::make_unique<NamedType>();
        n->path.push_back("<unsupported-type-in-quote>");
        return with_range(std::move(n), t);
    }
    }
}

PatternPtr clone(const Pattern& p) {
    switch (p.kind) {
    case NodeKind::WildcardPat:
        return with_range(std::make_unique<WildcardPat>(), p);
    case NodeKind::LiteralPat: {
        auto n = std::make_unique<LiteralPat>();
        n->literal = cl(static_cast<const LiteralPat&>(p).literal);
        return with_range(std::move(n), p);
    }
    case NodeKind::BindPat: {
        auto n = std::make_unique<BindPat>();
        n->name = static_cast<const BindPat&>(p).name;
        return with_range(std::move(n), p);
    }
    case NodeKind::IdentPat: {
        auto n = std::make_unique<IdentPat>();
        n->name = static_cast<const IdentPat&>(p).name;
        return with_range(std::move(n), p);
    }
    case NodeKind::EnumPat: {
        const auto& ep = static_cast<const EnumPat&>(p);
        auto n = std::make_unique<EnumPat>();
        n->case_name = ep.case_name;
        for (const auto& c : ep.children) {
            n->children.push_back(clp(c));
        }
        return with_range(std::move(n), p);
    }
    case NodeKind::TuplePat: {
        const auto& tp = static_cast<const TuplePat&>(p);
        auto n = std::make_unique<TuplePat>();
        for (const auto& el : tp.elements) {
            n->elements.push_back(clp(el));
        }
        return with_range(std::move(n), p);
    }
    case NodeKind::RangePat: {
        const auto& rp = static_cast<const RangePat&>(p);
        auto n = std::make_unique<RangePat>();
        n->low = cl(rp.low);
        n->high = cl(rp.high);
        n->inclusive = rp.inclusive;
        return with_range(std::move(n), p);
    }
    case NodeKind::OrPat: {
        const auto& op = static_cast<const OrPat&>(p);
        auto n = std::make_unique<OrPat>();
        for (const auto& alt : op.alternatives) {
            n->alternatives.push_back(clp(alt));
        }
        return with_range(std::move(n), p);
    }
    default:
        return with_range(std::make_unique<WildcardPat>(), p);
    }
}

DeclPtr clone(const Decl& d) {
    switch (d.kind) {
    case NodeKind::Func: {
        const auto& f = static_cast<const FuncDecl&>(d);
        auto n = std::make_unique<FuncDecl>();
        n->attributes = clone_attrs(f.attributes);
        n->visibility = f.visibility;
        n->is_async = f.is_async;
        n->is_comptime = f.is_comptime;
        n->recv_mode = f.recv_mode;
        n->name = f.name;
        n->name_splice = cl(f.name_splice);
        n->generics = clone_generics(f.generics);
        n->params = clone_params(f.params);
        n->effects = clone_effects(f.effects);
        n->result = clt(f.result);
        n->body = cl(f.body);
        return with_range(std::move(n), d);
    }
    case NodeKind::Struct: {
        const auto& s = static_cast<const StructDecl&>(d);
        auto n = std::make_unique<StructDecl>();
        n->attributes = clone_attrs(s.attributes);
        n->visibility = s.visibility;
        n->is_linear = s.is_linear;
        n->name = s.name;
        n->generics = clone_generics(s.generics);
        for (const auto& fld : s.fields) {
            StructDecl::Field nf;
            nf.attributes = clone_attrs(fld.attributes);
            nf.kind = fld.kind;
            nf.name = fld.name;
            nf.type = clt(fld.type);
            nf.range = fld.range;
            n->fields.push_back(std::move(nf));
        }
        for (const auto& m : s.methods) {
            n->methods.push_back(cld(m));
        }
        n->deinit_body = cl(s.deinit_body);
        return with_range(std::move(n), d);
    }
    case NodeKind::Enum: {
        const auto& e = static_cast<const EnumDecl&>(d);
        auto n = std::make_unique<EnumDecl>();
        n->attributes = clone_attrs(e.attributes);
        n->visibility = e.visibility;
        n->name = e.name;
        n->generics = clone_generics(e.generics);
        for (const auto& c : e.cases) {
            EnumDecl::Case nc;
            nc.name = c.name;
            for (const auto& [pn, pt] : c.payload) {
                nc.payload.emplace_back(pn, clt(pt));
            }
            nc.range = c.range;
            n->cases.push_back(std::move(nc));
        }
        for (const auto& m : e.methods) {
            n->methods.push_back(cld(m));
        }
        return with_range(std::move(n), d);
    }
    case NodeKind::Const: {
        const auto& c = static_cast<const ConstDecl&>(d);
        auto n = std::make_unique<ConstDecl>();
        n->visibility = c.visibility;
        n->name = c.name;
        n->type = clt(c.type);
        n->value = cl(c.value);
        return with_range(std::move(n), d);
    }
    case NodeKind::Extension: {
        // §12.4 macros generate `extension T { … }` blocks; the cloner must
        // reproduce them (target type, conformances, member decls) so the
        // declaration quote materializes a real extension, not a placeholder.
        const auto& e = static_cast<const ExtensionDecl&>(d);
        auto n = std::make_unique<ExtensionDecl>();
        n->attributes = clone_attrs(e.attributes);
        n->visibility = e.visibility;
        n->target = clt(e.target);
        for (const auto& c : e.conformances) {
            n->conformances.push_back(clt(c));
        }
        for (const auto& m : e.members) {
            n->members.push_back(cld(m));
        }
        return with_range(std::move(n), d);
    }
    default: {
        // Unsupported decl kind in a v0.5 quote — a const placeholder keeps
        // the tree well-formed; the resolver will surface the bad name.
        auto n = std::make_unique<ConstDecl>();
        n->name = "<unsupported-decl-in-quote>";
        return with_range(std::move(n), d);
    }
    }
}

}  // namespace vestra::ast
