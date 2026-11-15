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
            struct_ = static_cast<const ast::StructDecl*>(d.get());
            for (const auto& m : struct_->methods) {
                if (m->kind == ast::NodeKind::Func) {
                    check_func(static_cast<const ast::FuncDecl&>(*m));
                }
            }
            struct_ = nullptr;
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
    case TypeKind::MmioView:    // Annex §10.5 / §14.11: bound by the Mmio discharge
    case TypeKind::MmioRegion:
        return true;
    case TypeKind::Optional:
    case TypeKind::Vector:
    case TypeKind::Vec:  // an owned container of views carries their sources
        return is_view(t->inner(), depth + 1);
    case TypeKind::HashMap:
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
            && (n.path[0] == "Span" || n.path[0] == "MutSpan" || n.path[0] == "Str"
                || n.path[0] == "MmioView" || n.path[0] == "MmioRegion")) {
            return true;
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
    raw_depths_.clear();
    mmio_depths_.clear();
    decl_depth_.clear();
    depth_ = 1;
    awaits_ = 0;
    view_at_.clear();
    await_reported_.clear();
    view_params_.clear();
    if (f.is_async) {
        // A view parameter is live from the start.
        for (const auto& p : f.params) {
            if (ast_is_view(p.type.get())) {
                view_params_.insert(p.name);
            }
        }
    }
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
        scan(*value);
        check_expr(*value);
        const auto* sym = resolution_->binding_symbol(&s);
        if (sym != nullptr) {
            decl_depth_[sym] = depth_;
            if (func_ != nullptr && func_->is_async && is_view(sym->type)) {
                view_at_[sym] = awaits_;
            }
        }
        if (sym != nullptr && is_view(sym->type)) {
            locals_[sym] = origins(*value);
            check_store(sym, depth_, locals_[sym], value->range);
        }
        break;
    }
    case ast::NodeKind::AssignStmt: {
        const auto& a = static_cast<const ast::AssignStmt&>(s);
        if (a.value != nullptr) {
            scan(*a.value);
            check_expr(*a.value);
        }
        if (a.target != nullptr && strip(*a.target).kind == ast::NodeKind::IdentExpr) {
            // Reassigning a view binding makes it fresh.
            if (const auto* sym = resolution_->symbol_of(&strip(*a.target));
                sym != nullptr && view_at_.contains(sym)) {
                view_at_[sym] = awaits_;
            }
        } else if (a.target != nullptr) {
            scan(*a.target);
            if (a.value != nullptr) {
                check_place_store(*a.target, *a.value, s.range);
            }
        }
        if (a.target != nullptr && strip(*a.target).kind == ast::NodeKind::IdentExpr) {
            const auto* sym = resolution_->symbol_of(&strip(*a.target));
            if (sym != nullptr && locals_.contains(sym) && a.value != nullptr) {
                locals_[sym] = origins(*a.value);
                auto dd = decl_depth_.find(sym);
                check_store(sym, dd != decl_depth_.end() ? dd->second : 1, locals_[sym], s.range);
            }
        }
        break;
    }
    case ast::NodeKind::ReturnStmt: {
        const auto& r = static_cast<const ast::ReturnStmt&>(s);
        if (r.value != nullptr) {
            scan(*r.value);
            check_expr(*r.value);
            if (result_is_view_) {
                check_return(*r.value, s.range);
            }
        }
        break;
    }
    case ast::NodeKind::ExprStmt: {
        const auto& x = *static_cast<const ast::ExprStmt&>(s).expr;
        scan(x);
        check_expr(x);
        if (strip(x).kind == ast::NodeKind::CallExpr) {
            check_container_write(static_cast<const ast::CallExpr&>(strip(x)));
        }
        break;
    }
    case ast::NodeKind::WhileStmt: {
        const auto& w = static_cast<const ast::WhileStmt&>(s);
        scan(*w.cond);
        check_expr(*w.cond);
        if (w.body) {
            check_expr(*w.body);
        }
        break;
    }
    case ast::NodeKind::ForStmt: {
        const auto& f = static_cast<const ast::ForStmt&>(s);
        scan(*f.iter);
        check_expr(*f.iter);
        // An element of a container of views views what the container does.
        if (const auto* sym = resolution_->binding_symbol(&s);
            sym != nullptr && is_view(sym->type)) {
            locals_[sym] = origins(*f.iter);
            decl_depth_[sym] = depth_ + 1;
        }
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
            raw_depths_.push_back(depth_ + 1);
        }
        if (mmio && w.body) {
            mmio_blocks_.push_back(w.body->range);
            mmio_depths_.push_back(depth_ + 1);
        }
        if (w.body) {
            check_expr(*w.body);
        }
        if (raw && w.body) {
            raw_blocks_.pop_back();
            raw_depths_.pop_back();
        }
        if (mmio && w.body) {
            mmio_blocks_.pop_back();
            mmio_depths_.pop_back();
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
        ++depth_;
        for (const auto& st : static_cast<const ast::BlockExpr&>(e).stmts) {
            check_stmt(*st);
        }
        --depth_;
        break;
    case ast::NodeKind::IfExpr: {
        const auto& i = static_cast<const ast::IfExpr&>(e);
        if (i.cond) {
            scan(*i.cond);
            check_expr(*i.cond);
        }
        if (i.let_init) {
            scan(*i.let_init);
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
        scan(*m.scrutinee);
        check_expr(*m.scrutinee);
        for (const auto& arm : m.arms) {
            if (arm.body) {
                if (arm.body->kind != ast::NodeKind::BlockExpr) {
                    scan(*arm.body);
                }
                check_expr(*arm.body);
                // An arm is one expression, so `case .a: xs.push(v)` is a
                // container write in statement position.
                if (strip(*arm.body).kind == ast::NodeKind::CallExpr) {
                    check_container_write(static_cast<const ast::CallExpr&>(strip(*arm.body)));
                }
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

// §11.4 walk an expression in evaluation order (not into nested blocks, which
// check_expr walks statement by statement): count `await`s, and flag a view
// binding used after one that came after the binding was made.
void EscapeChecker::scan(const ast::Expr& e) {
    if (func_ == nullptr || !func_->is_async) {
        return;
    }
    switch (e.kind) {
    case ast::NodeKind::IdentExpr: {
        const auto* sym = resolution_->symbol_of(&e);
        if (sym == nullptr || await_reported_.contains(sym)) {
            return;
        }
        int made = -1;
        if (auto it = view_at_.find(sym); it != view_at_.end()) {
            made = it->second;
        } else if (sym->kind == SymbolKind::Parameter
                   && view_params_.contains(static_cast<const ast::IdentExpr&>(e).name)) {
            made = 0;
        }
        if (made >= 0 && awaits_ > made) {
            await_reported_.insert(sym);
            reporter_->report(
                diag::Diagnostic::error(std::format("'{}' is a view held across an `await`: a "
                                                    "suspended task keeps only owned values "
                                                    "(§11.4)",
                                                    sym->name))
                    .at(e.range));
        }
        return;
    }
    case ast::NodeKind::AwaitExpr:
        scan(*static_cast<const ast::AwaitExpr&>(e).inner);
        ++awaits_;
        return;
    case ast::NodeKind::ParenExpr:
        scan(*static_cast<const ast::ParenExpr&>(e).inner);
        return;
    case ast::NodeKind::UnaryExpr:
        scan(*static_cast<const ast::UnaryExpr&>(e).operand);
        return;
    case ast::NodeKind::BinaryExpr:
        scan(*static_cast<const ast::BinaryExpr&>(e).lhs);
        scan(*static_cast<const ast::BinaryExpr&>(e).rhs);
        return;
    case ast::NodeKind::CallExpr: {
        const auto& c = static_cast<const ast::CallExpr&>(e);
        if (c.callee) {
            scan(*c.callee);
        }
        for (const auto& a : c.args) {
            if (a.value) {
                scan(*a.value);
            }
        }
        return;
    }
    case ast::NodeKind::MemberExpr:
        scan(*static_cast<const ast::MemberExpr&>(e).base);
        return;
    case ast::NodeKind::IndexExpr: {
        const auto& ix = static_cast<const ast::IndexExpr&>(e);
        scan(*ix.base);
        for (const auto& i : ix.indices) {
            scan(*i);
        }
        return;
    }
    case ast::NodeKind::CopyExpr:
        scan(*static_cast<const ast::CopyExpr&>(e).inner);
        return;
    case ast::NodeKind::TryExpr:
        scan(*static_cast<const ast::TryExpr&>(e).inner);
        return;
    case ast::NodeKind::SpawnExpr:
        scan(*static_cast<const ast::SpawnExpr&>(e).inner);
        return;
    case ast::NodeKind::AsExpr:
        scan(*static_cast<const ast::AsExpr&>(e).value);
        return;
    case ast::NodeKind::VectorLitExpr:
        for (const auto& el : static_cast<const ast::VectorLitExpr&>(e).elements) {
            scan(*el);
        }
        return;
    case ast::NodeKind::TupleLitExpr:
        for (const auto& el : static_cast<const ast::TupleLitExpr&>(e).elements) {
            scan(*el);
        }
        return;
    default:
        return;  // blocks, if, match: their statements are scanned in turn
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

void EscapeChecker::check_store(const Symbol* target,
                                int depth,
                                const Origins& o,
                                diag::SourceRange site) {
    for (const auto& og : o) {
        switch (og.kind) {
        case Origin::Kind::Local:
            if (og.depth > depth && og.sym != nullptr) {
                reporter_->report(
                    diag::Diagnostic::error(std::format("'{}' would outlive '{}', which its view "
                                                        "comes from: '{}' ends with an inner block",
                                                        target->name,
                                                        og.sym->name,
                                                        og.sym->name))
                        .at(site));
                return;
            }
            break;
        case Origin::Kind::Temp:
            reporter_->report(
                diag::Diagnostic::error(std::format("'{}' would hold a view of a temporary, which "
                                                    "ends with this statement",
                                                    target->name))
                    .at(site));
            return;
        case Origin::Kind::Raw:
            if (og.depth > depth) {
                reporter_->report(
                    diag::Diagnostic::error(std::format("'{}' would outlive the `with RawMemory` "
                                                        "block that minted its view (Annex §10.5)",
                                                        target->name))
                        .at(site));
                return;
            }
            break;
        case Origin::Kind::Mmio:
            if (og.depth > depth) {
                reporter_->report(
                    diag::Diagnostic::error(std::format("'{}' would outlive the `with Mmio` block "
                                                        "that minted its view (Annex §14.11)",
                                                        target->name))
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
            auto dd = decl_depth_.find(sym);
            return {Origin{Origin::Kind::Local, sym, {}, dd != decl_depth_.end() ? dd->second : 1}};
        }
        // A `sink` parameter is the callee's own value; a `read` / `inout`
        // one is the caller's, which outlives the call.
        if (sym->kind == SymbolKind::Parameter && func_ != nullptr) {
            for (const auto& p : func_->params) {
                if (p.name == sym->name && p.mode == ast::ParamMode::Sink) {
                    return {Origin{Origin::Kind::Local, sym, {}, 1}};
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
    case ast::NodeKind::VectorLitExpr:
    case ast::NodeKind::TupleLitExpr: {
        // A literal of views holds each element's sources.
        const auto& els = e.kind == ast::NodeKind::VectorLitExpr
                              ? static_cast<const ast::VectorLitExpr&>(e).elements
                              : static_cast<const ast::TupleLitExpr&>(e).elements;
        Origins out;
        for (const auto& el : els) {
            auto et = resolution_->type_of(el.get());
            if (is_view(et) || (et != nullptr && et->kind() == TypeKind::String)) {
                auto o = origins(*el);
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
                           raw_blocks_.empty() ? diag::SourceRange{} : raw_blocks_.back(),
                           raw_depths_.empty() ? 0 : raw_depths_.back()}};
        }
        if (callee_is(c, "MmioView", "at") || callee_is(c, "MmioRegion", "at")
            || callee_is(c, "MmioWireView", "at")) {
            return {Origin{Origin::Kind::Mmio,
                           nullptr,
                           mmio_blocks_.empty() ? diag::SourceRange{} : mmio_blocks_.back(),
                           mmio_depths_.empty() ? 0 : mmio_depths_.back()}};
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

// A local container of views (`var xs: Vec[Str]`) named by `e`.
const Symbol* EscapeChecker::container_local(const ast::Expr& e) const {
    const auto& b = strip(e);
    if (b.kind != ast::NodeKind::IdentExpr) {
        return nullptr;
    }
    const auto* sym = resolution_->symbol_of(&b);
    if (sym == nullptr || !locals_.contains(sym) || sym->type == nullptr
        || (sym->type->kind() != TypeKind::Vec && sym->type->kind() != TypeKind::HashMap)) {
        return nullptr;
    }
    return sym;
}

// The slot argument `i` of a write lands in: a Vec's element, or a
// HashMap's key (0) or value (1).
TypePtr EscapeChecker::element_slot(TypePtr container, std::size_t i) const {
    if (container == nullptr) {
        return nullptr;
    }
    if (container->kind() == TypeKind::HashMap) {
        return i < container->parts().size() ? container->parts()[i] : nullptr;
    }
    return container->inner();
}

// Whether `arg` stores a view into `slot`: it is one, or it is owned storage
// a view slot borrows (a String into a Str, a Vec or array into a Span).
bool EscapeChecker::flows_into(const ast::Expr& arg, TypePtr slot) const {
    if (!is_view(slot)) {
        return false;
    }
    auto at = resolution_->type_of(&arg);
    if (at == nullptr) {
        return false;
    }
    if (is_view(at)) {
        return true;
    }
    if (slot->kind() == TypeKind::Str) {
        return at->kind() == TypeKind::String;
    }
    if (slot->kind() == TypeKind::Span || slot->kind() == TypeKind::MutSpan) {
        return at->kind() == TypeKind::Vec || at->kind() == TypeKind::Vector;
    }
    return false;
}

void EscapeChecker::add_sources(const Symbol* sym, const Origins& o, diag::SourceRange site) {
    auto& cur = locals_[sym];
    cur.insert(cur.end(), o.begin(), o.end());
    auto dd = decl_depth_.find(sym);
    check_store(sym, dd != decl_depth_.end() ? dd->second : 1, o, site);
}

// §19.4 `xs.push(v)`, `m.insert(k, v)`, `xs.extend(ys)`: a view written into
// a local container joins the container's sources.
void EscapeChecker::check_container_write(const ast::CallExpr& c) {
    if (c.callee == nullptr || c.callee->kind != ast::NodeKind::MemberExpr) {
        return;
    }
    const auto& m = static_cast<const ast::MemberExpr&>(*c.callee);
    const auto* sym = container_local(*m.base);
    // A container the caller owns (`self.items`, an `inout` parameter)
    // outlives this function.
    TypePtr ct = sym != nullptr ? sym->type : nullptr;
    const std::string root = sym == nullptr ? outliving_root(*m.base, ct) : std::string{};
    if (sym == nullptr && root.empty()) {
        return;
    }
    const bool map = ct->kind() == TypeKind::HashMap;
    for (std::size_t i = 0; i < c.args.size(); ++i) {
        const auto* a = c.args[i].value.get();
        if (a == nullptr) {
            continue;
        }
        // A whole container appended brings its own sources.
        auto at = resolution_->type_of(a);
        bool flows = at != nullptr && at->kind() == ct->kind() && is_view(at);
        flows = flows || flows_into(*a, element_slot(ct, map ? i : 0));
        if (!flows) {
            continue;
        }
        if (sym != nullptr) {
            add_sources(sym, origins(*a), c.range);
        } else {
            check_outliving_store(root, origins(*a), c.range);
        }
    }
}

// The caller-owned root of a container place (`self`, `self.items`, an
// `inout` / `read` parameter), or "" when it is local or not a container of
// views.
// A place's type. `self.field`: the resolver leaves `self` untyped, so read
// the field's declared type off the enclosing struct.
TypePtr EscapeChecker::place_type(const ast::Expr& e) const {
    auto t = resolution_->type_of(&strip(e));
    if ((t == nullptr || t->is_error()) && strip(e).kind == ast::NodeKind::MemberExpr
        && struct_ != nullptr) {
        const auto& m = static_cast<const ast::MemberExpr&>(strip(e));
        if (m.base != nullptr && strip(*m.base).kind == ast::NodeKind::SelfExpr) {
            for (const auto& f : struct_->fields) {
                if (f.name == m.member) {
                    t = resolution_->types()->member_type(f.type.get());
                }
            }
        }
    }
    return t;
}

// The binding a place (`a.b[i].c`) is rooted in.
const ast::Expr& EscapeChecker::place_root(const ast::Expr& e) {
    const ast::Expr* p = &strip(e);
    while (true) {
        if (p->kind == ast::NodeKind::MemberExpr) {
            p = &strip(*static_cast<const ast::MemberExpr*>(p)->base);
        } else if (p->kind == ast::NodeKind::IndexExpr) {
            p = &strip(*static_cast<const ast::IndexExpr*>(p)->base);
        } else {
            return *p;
        }
    }
}

// §19.4 `w.name = v`, `self.items = v`, `xs[i] = v`: a view stored into a
// field or element joins a local root's sources, and may not outlive the
// function when the root is the caller's (`self`, a parameter).
void EscapeChecker::check_place_store(const ast::Expr& target,
                                      const ast::Expr& value,
                                      diag::SourceRange site) {
    const auto& t = strip(target);
    TypePtr slot;
    if (t.kind == ast::NodeKind::IndexExpr) {
        slot = element_slot(place_type(*static_cast<const ast::IndexExpr&>(t).base), 1);
    } else if (t.kind == ast::NodeKind::MemberExpr) {
        slot = place_type(t);
    } else {
        return;
    }
    if (!flows_into(value, slot)) {
        return;
    }
    const auto& root = place_root(t);
    if (root.kind == ast::NodeKind::SelfExpr) {
        check_outliving_store("self", origins(value), site);
        return;
    }
    if (root.kind != ast::NodeKind::IdentExpr) {
        return;
    }
    const auto* sym = resolution_->symbol_of(&root);
    if (sym != nullptr && sym->kind == SymbolKind::Local && locals_.contains(sym)) {
        add_sources(sym, origins(value), site);
    } else if (auto name = caller_root(root); !name.empty()) {
        check_outliving_store(name, origins(value), site);
    }
}

std::string EscapeChecker::outliving_root(const ast::Expr& e, TypePtr& out) const {
    auto t = place_type(e);
    out = t;
    if (t == nullptr || (t->kind() != TypeKind::Vec && t->kind() != TypeKind::HashMap)
        || !is_view(t)) {
        return {};
    }
    const ast::Expr* p = &place_root(e);
    if (p->kind == ast::NodeKind::SelfExpr) {
        return "self";
    }
    return caller_root(*p);
}

// The name of a non-`sink` parameter `e` names (the caller's storage), or "".
std::string EscapeChecker::caller_root(const ast::Expr& e) const {
    const ast::Expr* p = &e;
    if (p->kind != ast::NodeKind::IdentExpr || func_ == nullptr) {
        return {};
    }
    const auto* sym = resolution_->symbol_of(p);
    if (sym == nullptr || sym->kind != SymbolKind::Parameter) {
        return {};
    }
    for (const auto& prm : func_->params) {
        if (prm.name == sym->name && prm.mode == ast::ParamMode::Sink) {
            return {};
        }
    }
    return sym->name;
}

// §19.4 a view stored into a container that outlives the function: like a
// return, it may not view the function's own storage or a temporary.
void EscapeChecker::check_outliving_store(const std::string& target,
                                          const Origins& o,
                                          diag::SourceRange site) {
    for (const auto& og : o) {
        switch (og.kind) {
        case Origin::Kind::Local:
            if (og.sym != nullptr) {
                reporter_->report(
                    diag::Diagnostic::error(std::format("'{}' would outlive '{}', which its view "
                                                        "comes from: '{}' is local to '{}'",
                                                        target,
                                                        og.sym->name,
                                                        og.sym->name,
                                                        func_->name))
                        .at(site));
                return;
            }
            break;
        case Origin::Kind::Temp:
            reporter_->report(
                diag::Diagnostic::error(std::format("'{}' would hold a view of a temporary, which "
                                                    "ends with this statement",
                                                    target))
                    .at(site));
            return;
        case Origin::Kind::Raw:
            if (og.block.is_valid()) {
                reporter_->report(
                    diag::Diagnostic::error(std::format("'{}' would outlive the `with RawMemory` "
                                                        "block that minted its view (Annex §10.5)",
                                                        target))
                        .at(site));
                return;
            }
            break;
        case Origin::Kind::Mmio:
            if (og.block.is_valid()) {
                reporter_->report(
                    diag::Diagnostic::error(std::format("'{}' would outlive the `with Mmio` block "
                                                        "that minted its view (Annex §14.11)",
                                                        target))
                        .at(site));
                return;
            }
            break;
        }
    }
}

}  // namespace vestra::sema
