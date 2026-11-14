// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#include "vestra/sema/escape.hpp"

#include <format>

namespace vestra::sema {

namespace {

const ast::Expr& strip(const ast::Expr& e) {
    const ast::Expr* p = &e;
    while (p->kind == ast::NodeKind::ParenExpr) {
        p = static_cast<const ast::ParenExpr&>(*p).inner.get();
    }
    return *p;
}

bool callee_is(const ast::CallExpr& c, std::string_view ns, std::string_view member) {
    if (c.callee == nullptr || c.callee->kind != ast::NodeKind::MemberExpr) {
        return false;
    }
    const auto& m = static_cast<const ast::MemberExpr&>(*c.callee);
    return m.member == member && m.base != nullptr && m.base->kind == ast::NodeKind::IdentExpr
           && static_cast<const ast::IdentExpr&>(*m.base).name == ns;
}

// The value a block (or a bare expression) produces: its trailing expression.
const ast::Expr* tail_of(const ast::Expr* e) {
    if (e == nullptr) {
        return nullptr;
    }
    if (e->kind != ast::NodeKind::BlockExpr) {
        return e;
    }
    const auto& b = static_cast<const ast::BlockExpr&>(*e);
    if (b.stmts.empty() || b.stmts.back()->kind != ast::NodeKind::ExprStmt) {
        return nullptr;
    }
    return static_cast<const ast::ExprStmt&>(*b.stmts.back()).expr.get();
}

}  // namespace

void EscapeChecker::check() {
    for (const auto& d : unit_->decls) {
        if (resolution_->is_gated_out(d.get())) {
            continue;
        }
        if (d->kind == ast::NodeKind::Func) {
            check_func(static_cast<const ast::FuncDecl&>(*d));
        } else if (d->kind == ast::NodeKind::Struct) {
            for (const auto& m : static_cast<const ast::StructDecl&>(*d).methods) {
                if (m->kind == ast::NodeKind::Func) {
                    check_func(static_cast<const ast::FuncDecl&>(*m));
                }
            }
        } else if (d->kind == ast::NodeKind::Enum) {
            for (const auto& m : static_cast<const ast::EnumDecl&>(*d).methods) {
                if (m->kind == ast::NodeKind::Func) {
                    check_func(static_cast<const ast::FuncDecl&>(*m));
                }
            }
        }
    }
}

const ast::StructDecl* EscapeChecker::unit_struct(std::string_view name) const {
    for (const auto& d : unit_->decls) {
        if (d->kind == ast::NodeKind::Struct
            && static_cast<const ast::StructDecl&>(*d).name == name) {
            return static_cast<const ast::StructDecl*>(d.get());
        }
    }
    return nullptr;
}

bool EscapeChecker::is_view(TypePtr t, int depth) const {
    if (t == nullptr || depth > 16) {
        return false;
    }
    switch (t->kind()) {
    case TypeKind::Span:
    case TypeKind::MutSpan:
    case TypeKind::Str:
        return true;
    case TypeKind::Optional:
    case TypeKind::Vector:
        return is_view(t->inner(), depth + 1);
    case TypeKind::Tuple:
        for (auto p : t->parts()) {
            if (is_view(p, depth + 1)) {
                return true;
            }
        }
        return false;
    case TypeKind::Struct: {
        for (auto p : t->parts()) {
            if (is_view(p, depth + 1)) {
                return true;
            }
        }
        const auto* d = t->nominal_decl();
        if (d == nullptr || d->kind != ast::NodeKind::Struct) {
            return false;
        }
        for (const auto& f : static_cast<const ast::StructDecl*>(d)->fields) {
            if (auto ft = resolution_->types()->member_type(f.type.get())) {
                if (is_view(ft, depth + 1)) {
                    return true;
                }
            } else if (ast_is_view(f.type.get(), depth + 1)) {
                return true;
            }
        }
        return false;
    }
    default:
        return false;
    }
}

bool EscapeChecker::ast_is_view(const ast::Type* t, int depth) const {
    if (t == nullptr || depth > 16) {
        return false;
    }
    switch (t->kind) {
    case ast::NodeKind::NamedType: {
        const auto& n = static_cast<const ast::NamedType&>(*t);
        if (n.path.size() == 1
            && (n.path[0] == "Span" || n.path[0] == "MutSpan" || n.path[0] == "Str")) {
            return true;
        }
        // Owned containers of views (`Vec[Str]`, as `split` and `args()`
        // return) are admitted for now; see the Box / Channel rule.
        if (n.path.size() == 1 && (n.path[0] == "Vec" || n.path[0] == "HashMap")) {
            return false;
        }
        for (const auto& a : n.type_args) {
            if (ast_is_view(a.get(), depth + 1)) {
                return true;
            }
        }
        if (n.path.size() == 1) {
            if (const auto* s = unit_struct(n.path[0])) {
                for (const auto& f : s->fields) {
                    if (ast_is_view(f.type.get(), depth + 1)) {
                        return true;
                    }
                }
            }
        }
        return false;
    }
    case ast::NodeKind::OptionalType:
        return ast_is_view(static_cast<const ast::OptionalType&>(*t).inner.get(), depth + 1);
    case ast::NodeKind::VectorType:
        return ast_is_view(static_cast<const ast::VectorType&>(*t).element.get(), depth + 1);
    case ast::NodeKind::TupleType:
        for (const auto& el : static_cast<const ast::TupleType&>(*t).elements) {
            if (ast_is_view(el.get(), depth + 1)) {
                return true;
            }
        }
        return false;
    default:
        return false;
    }
}

void EscapeChecker::check_func(const ast::FuncDecl& f) {
    if (f.body == nullptr) {
        return;
    }
    func_ = &f;
    locals_.clear();
    raw_blocks_.clear();
    mmio_blocks_.clear();
    result_is_view_ = ast_is_view(f.result.get());
    // §11.1 a Future can't outlive the scope that spawned it.
    if (f.result != nullptr && f.result->kind == ast::NodeKind::NamedType) {
        const auto& n = static_cast<const ast::NamedType&>(*f.result);
        if (n.path.size() == 1 && n.path[0] == "Future") {
            reporter_->report(diag::Diagnostic::error(
                                  std::format("'{}' can't return a Future: a Future can't outlive "
                                              "the scope that spawned it",
                                              f.name))
                                  .at(f.result->range));
        }
    }
    if (f.body->kind == ast::NodeKind::BlockExpr) {
        const auto& b = static_cast<const ast::BlockExpr&>(*f.body);
        for (std::size_t i = 0; i < b.stmts.size(); ++i) {
            check_stmt(*b.stmts[i]);
        }
        // A trailing value is an implicit return.
        if (result_is_view_) {
            if (const auto* t = tail_of(f.body.get())) {
                check_return(*t, t->range);
            }
        }
    } else {
        check_expr(*f.body);
    }
    func_ = nullptr;
}

void EscapeChecker::check_stmt(const ast::Stmt& s) {
    switch (s.kind) {
    case ast::NodeKind::LetStmt:
    case ast::NodeKind::VarStmt: {
        const ast::Expr* value = s.kind == ast::NodeKind::LetStmt
                                     ? static_cast<const ast::LetStmt&>(s).value.get()
                                     : static_cast<const ast::VarStmt&>(s).value.get();
        if (value == nullptr) {
            break;
        }
        check_expr(*value);
        const auto* sym = resolution_->binding_symbol(&s);
        if (sym != nullptr && is_view(sym->type)) {
            locals_[sym] = origins(*value);
        }
        break;
    }
    case ast::NodeKind::AssignStmt: {
        const auto& a = static_cast<const ast::AssignStmt&>(s);
        if (a.value != nullptr) {
            check_expr(*a.value);
        }
        if (a.target != nullptr && strip(*a.target).kind == ast::NodeKind::IdentExpr) {
            const auto* sym = resolution_->symbol_of(&strip(*a.target));
            if (sym != nullptr && locals_.contains(sym) && a.value != nullptr) {
                locals_[sym] = origins(*a.value);
            }
        }
        break;
    }
    case ast::NodeKind::ReturnStmt: {
        const auto& r = static_cast<const ast::ReturnStmt&>(s);
        if (r.value != nullptr) {
            check_expr(*r.value);
            if (result_is_view_) {
                check_return(*r.value, s.range);
            }
        }
        break;
    }
    case ast::NodeKind::ExprStmt:
        check_expr(*static_cast<const ast::ExprStmt&>(s).expr);
        break;
    case ast::NodeKind::WhileStmt: {
        const auto& w = static_cast<const ast::WhileStmt&>(s);
        check_expr(*w.cond);
        if (w.body) {
            check_expr(*w.body);
        }
        break;
    }
    case ast::NodeKind::ForStmt: {
        const auto& f = static_cast<const ast::ForStmt&>(s);
        check_expr(*f.iter);
        if (f.body) {
            check_expr(*f.body);
        }
        break;
    }
    case ast::NodeKind::WithStmt: {
        const auto& w = static_cast<const ast::WithStmt&>(s);
        bool raw = false;
        bool mmio = false;
        for (const auto& b : w.bindings) {
            if (b.value) {
                check_expr(*b.value);
            }
            if (b.cap_type != nullptr && b.cap_type->kind == ast::NodeKind::NamedType) {
                const auto& n = static_cast<const ast::NamedType&>(*b.cap_type);
                raw = raw || (!n.path.empty() && n.path.back() == "RawMemory");
                mmio = mmio || (!n.path.empty() && n.path.back() == "Mmio");
            }
        }
        if (raw && w.body) {
            raw_blocks_.push_back(w.body->range);
        }
        if (mmio && w.body) {
            mmio_blocks_.push_back(w.body->range);
        }
        if (w.body) {
            check_expr(*w.body);
        }
        if (raw && w.body) {
            raw_blocks_.pop_back();
        }
        if (mmio && w.body) {
            mmio_blocks_.pop_back();
        }
        break;
    }
    default:
        break;
    }
}

void EscapeChecker::check_expr(const ast::Expr& e) {
    switch (e.kind) {
    case ast::NodeKind::BlockExpr:
        for (const auto& st : static_cast<const ast::BlockExpr&>(e).stmts) {
            check_stmt(*st);
        }
        break;
    case ast::NodeKind::IfExpr: {
        const auto& i = static_cast<const ast::IfExpr&>(e);
        if (i.cond) {
            check_expr(*i.cond);
        }
        if (i.let_init) {
            check_expr(*i.let_init);
        }
        if (i.then_branch) {
            check_expr(*i.then_branch);
        }
        if (i.else_branch) {
            check_expr(*i.else_branch);
        }
        break;
    }
    case ast::NodeKind::MatchExpr: {
        const auto& m = static_cast<const ast::MatchExpr&>(e);
        check_expr(*m.scrutinee);
        for (const auto& arm : m.arms) {
            if (arm.body) {
                check_expr(*arm.body);
            }
        }
        break;
    }
    case ast::NodeKind::DoCatchExpr: {
        const auto& dc = static_cast<const ast::DoCatchExpr&>(e);
        check_expr(*dc.do_body);
        check_expr(*dc.catch_body);
        break;
    }
    case ast::NodeKind::ParenExpr:
        check_expr(*static_cast<const ast::ParenExpr&>(e).inner);
        break;
    default:
        break;
    }
}

void EscapeChecker::check_return(const ast::Expr& value, diag::SourceRange site) {
    for (const auto& o : origins(value)) {
        switch (o.kind) {
        case Origin::Kind::Local:
            reporter_->report(
                diag::Diagnostic::error(std::format("can't return a view of '{}': it's local to "
                                                    "'{}', so the view would outlive it",
                                                    o.sym != nullptr ? o.sym->name : "?",
                                                    func_->name))
                    .at(site));
            return;
        case Origin::Kind::Temp:
            reporter_->report(
                diag::Diagnostic::error("can't return a view of a temporary: it ends with this "
                                        "statement, so the view would outlive it")
                    .at(site));
            return;
        case Origin::Kind::Raw:
            if (o.block.is_valid()) {
                reporter_->report(
                    diag::Diagnostic::error("can't return a view minted under `with RawMemory`: "
                                            "its lifetime ends with that block (Annex §10.5)")
                        .at(site));
                return;
            }
            break;
        case Origin::Kind::Mmio:
            if (o.block.is_valid()) {
                reporter_->report(
                    diag::Diagnostic::error("can't return an MMIO view minted under `with Mmio`: "
                                            "its lifetime ends with that block (Annex §14.11)")
                        .at(site));
                return;
            }
            break;
        }
    }
}

EscapeChecker::Origins EscapeChecker::storage_of(const ast::Expr& e0) const {
    const ast::Expr& e = strip(e0);
    switch (e.kind) {
    case ast::NodeKind::IdentExpr: {
        const auto* sym = resolution_->symbol_of(&e);
        if (sym == nullptr) {
            return {};
        }
        if (sym->kind == SymbolKind::Local) {
            if (auto it = locals_.find(sym); it != locals_.end()) {
                return it->second;  // a view local: what it views
            }
            return {Origin{Origin::Kind::Local, sym, {}}};
        }
        // A `sink` parameter is the callee's own value; a `read` / `inout`
        // one is the caller's, which outlives the call.
        if (sym->kind == SymbolKind::Parameter && func_ != nullptr) {
            for (const auto& p : func_->params) {
                if (p.name == sym->name && p.mode == ast::ParamMode::Sink) {
                    return {Origin{Origin::Kind::Local, sym, {}}};
                }
            }
        }
        return {};
    }
    case ast::NodeKind::MemberExpr:
        return storage_of(*static_cast<const ast::MemberExpr&>(e).base);
    case ast::NodeKind::IndexExpr:
        return storage_of(*static_cast<const ast::IndexExpr&>(e).base);
    case ast::NodeKind::SelfExpr:
    case ast::NodeKind::StringLit:
        return {};
    case ast::NodeKind::CallExpr:
    case ast::NodeKind::VectorLitExpr:
    case ast::NodeKind::InterpStringExpr:
    case ast::NodeKind::TupleLitExpr:
        return {Origin{Origin::Kind::Temp, nullptr, {}}};
    default:
        return {};
    }
}

EscapeChecker::Origins EscapeChecker::origins(const ast::Expr& e0) const {
    const ast::Expr& e = strip(e0);
    auto t = resolution_->type_of(&e);
    if (!is_view(t)) {
        return storage_of(e);  // a value coerced into a view: a view of it
    }
    switch (e.kind) {
    case ast::NodeKind::IdentExpr: {
        const auto* sym = resolution_->symbol_of(&e);
        if (sym != nullptr) {
            if (auto it = locals_.find(sym); it != locals_.end()) {
                return it->second;
            }
        }
        return {};  // a parameter, a static, a literal: outlives the call
    }
    case ast::NodeKind::MemberExpr:
        return origins(*static_cast<const ast::MemberExpr&>(e).base);
    case ast::NodeKind::IndexExpr:
        return origins(*static_cast<const ast::IndexExpr&>(e).base);
    case ast::NodeKind::IfExpr: {
        const auto& i = static_cast<const ast::IfExpr&>(e);
        Origins out;
        for (const auto* br : {tail_of(i.then_branch.get()), tail_of(i.else_branch.get())}) {
            if (br != nullptr) {
                auto o = origins(*br);
                out.insert(out.end(), o.begin(), o.end());
            }
        }
        return out;
    }
    case ast::NodeKind::MatchExpr: {
        Origins out;
        for (const auto& arm : static_cast<const ast::MatchExpr&>(e).arms) {
            if (const auto* br = tail_of(arm.body.get())) {
                auto o = origins(*br);
                out.insert(out.end(), o.begin(), o.end());
            }
        }
        return out;
    }
    case ast::NodeKind::CallExpr: {
        const auto& c = static_cast<const ast::CallExpr&>(e);
        // Annex §10.5: a raw-minted view lives as long as its discharge.
        if (callee_is(c, "Span", "raw") || callee_is(c, "MutSpan", "raw")) {
            return {Origin{Origin::Kind::Raw,
                           nullptr,
                           raw_blocks_.empty() ? diag::SourceRange{} : raw_blocks_.back()}};
        }
        if (callee_is(c, "MmioView", "at") || callee_is(c, "MmioRegion", "at")
            || callee_is(c, "MmioWireView", "at")) {
            return {Origin{Origin::Kind::Mmio,
                           nullptr,
                           mmio_blocks_.empty() ? diag::SourceRange{} : mmio_blocks_.back()}};
        }
        Origins out;
        // A projection (`s.slice(..)`, `xs.split(at:)`, a method of a view)
        // views what its receiver views.
        if (c.callee != nullptr && c.callee->kind == ast::NodeKind::MemberExpr) {
            const auto& m = static_cast<const ast::MemberExpr&>(*c.callee);
            if (resolution_->qualified_name_of(c.callee.get()) == nullptr
                && resolution_->type_of(m.base.get()) != nullptr) {
                auto o = origins(*m.base);
                out.insert(out.end(), o.begin(), o.end());
            }
        }
        // §19.4 a returned view's lifetime is the intersection of its view
        // inputs': it may come from any argument that is (or becomes) a view.
        const ast::FuncDecl* fn = nullptr;
        if (const auto* sym = resolution_->symbol_of(c.callee.get());
            sym != nullptr && sym->decl != nullptr && sym->decl->kind == ast::NodeKind::Func) {
            fn = static_cast<const ast::FuncDecl*>(sym->decl);
        }
        // A struct construction: a field of view type holds a view of its
        // argument.
        const ast::StructDecl* sd = nullptr;
        if (const auto* sym = resolution_->symbol_of(c.callee.get());
            sym != nullptr && sym->kind == SymbolKind::Struct && sym->decl != nullptr
            && sym->decl->kind == ast::NodeKind::Struct) {
            sd = static_cast<const ast::StructDecl*>(sym->decl);
        }
        for (std::size_t i = 0; i < c.args.size(); ++i) {
            const auto& a = c.args[i];
            if (a.value == nullptr) {
                continue;
            }
            bool view_param = fn != nullptr && i < fn->params.size()
                              && ast_is_view(fn->params[i].type.get());
            if (sd != nullptr) {
                for (const auto& f : sd->fields) {
                    if (f.name == a.label && ast_is_view(f.type.get())) {
                        view_param = true;
                    }
                }
            }
            if (view_param || is_view(resolution_->type_of(a.value.get()))) {
                auto o = origins(*a.value);
                out.insert(out.end(), o.begin(), o.end());
            }
        }
        return out;
    }
    default:
        return {};
    }
}

}  // namespace vestra::sema
