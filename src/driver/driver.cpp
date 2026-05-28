// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#include "vestra/driver/driver.hpp"

#include "vestra/ast/printer.hpp"
#include "vestra/codegen/cpp_emitter.hpp"
#include "vestra/diag/diagnostic.hpp"
#include "vestra/diag/source_manager.hpp"
#include "vestra/lex/lexer.hpp"
#include "vestra/lex/token.hpp"
#include "vestra/parse/parser.hpp"
#include "vestra/sema/capability.hpp"
#include "vestra/sema/exclusivity.hpp"
#include "vestra/sema/ownership.hpp"
#include "vestra/sema/resolver.hpp"
#include "vestra/sema/types.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <ostream>
#include <string>
#include <string_view>

namespace vestra::driver {

namespace {

constexpr std::string_view UsageText = R"(usage: vestra <subcommand> [options]

Subcommands:
  build <file.vst> [-o DIR] [--emit-only] [--dump-ast] [--dump-tokens]
                   [--skip-check] [--no-libc]
                   [--target=ARCH] [--target-features=LIST]
        Parse, semantically check, then transpile a Vestra source file to
        C++ (.hpp + .cpp) under DIR. --skip-check elides sema (debug aid).
        --no-libc marks the build as freestanding (§A10 §15.5): the
        generated header opens with `// vestra: no_libc = true`.
        --target picks the target architecture (host / aarch64 / x86_64 /
        riscv64; default host). --target-features takes a comma-list of
        ISA features (e.g. lse2,cx16). Wide atomics (Atomic[UInt128] /
        AtomicTaggedPointer[T]) require the matching feature when the
        target is non-host: lse2 on aarch64, cx16 on x86_64 (§14.9.4).
  check <file.vst>
        Parse and run name resolution + type checking; print any diagnostics.
        Exits 0 on a clean check.
  fmt <file.vst>
        Pretty-print a Vestra source file to stdout.
  audit <file.vst> [--sysreg] [--no-libc]
        Enumerate every site that crossed a discipline-bearing
        boundary so a cross-architecture review can verify the
        build's contracts at the call site.
        --sysreg : every Sysreg.<name>.{read|write} access plus
                   whether the write carries an auto-emitted
                   post-write barrier (§14.12.3).
        --no-libc: every `@extern + @symbol` declaration so a
                   zero-finding pass under the freestanding
                   profile (§15.5) is the build-time proof the
                   binary is libc-free.
  help
        Show this message.

The build subcommand refuses to emit C++ when sema reports errors. Use
--skip-check to bypass that gate while debugging the emitter.
)";

void print_usage(std::ostream& os) {
    os << UsageText;
}

}  // namespace

int run(std::span<const std::string_view> argv, std::ostream& out, std::ostream& err) {
    if (argv.size() < 2 || argv[1] == "help" || argv[1] == "-h" || argv[1] == "--help") {
        print_usage(out);
        return 0;
    }
    auto sub = argv[1];
    if (sub == "build") {
        BuildOptions opts;
        for (std::size_t i = 2; i < argv.size(); ++i) {
            auto a = argv[i];
            if (a == "-o" && i + 1 < argv.size()) {
                opts.out_dir = std::string{argv[++i]};
            } else if (a == "--emit-only") {
                opts.emit_only = true;
            } else if (a == "--dump-ast") {
                opts.dump_ast = true;
            } else if (a == "--dump-tokens") {
                opts.dump_tokens = true;
            } else if (a == "--skip-check") {
                opts.skip_check = true;
            } else if (a == "--no-libc") {
                opts.no_libc = true;
            } else if (a.starts_with("--target=")) {
                opts.target = std::string{a.substr(std::string_view{"--target="}.size())};
            } else if (a.starts_with("--target-features=")) {
                // §14.9.4 — split the comma-list into the feature
                // set. Empty trailing entries from a stray comma
                // are dropped so `lse2,` and `lse2` mean the same.
                auto list = a.substr(std::string_view{"--target-features="}.size());
                std::size_t start = 0;
                while (start <= list.size()) {
                    auto comma = list.find(',', start);
                    auto chunk = list.substr(start,
                                             comma == std::string_view::npos ? list.size() - start
                                                                             : comma - start);
                    if (!chunk.empty()) {
                        opts.target_features.emplace_back(chunk);
                    }
                    if (comma == std::string_view::npos) {
                        break;
                    }
                    start = comma + 1;
                }
            } else if (!a.empty() && a[0] != '-') {
                opts.input = std::string{a};
            } else {
                err << "vestra: unknown option '" << a << "'\n";
                return 2;
            }
        }
        if (opts.input.empty()) {
            err << "vestra build: missing input file\n";
            print_usage(err);
            return 2;
        }
        return run_build(opts, out, err);
    }
    if (sub == "check") {
        if (argv.size() < 3) {
            err << "vestra check: missing input file\n";
            return 2;
        }
        return run_check(std::string{argv[2]}, out, err);
    }
    if (sub == "fmt") {
        if (argv.size() < 3) {
            err << "vestra fmt: missing input file\n";
            return 2;
        }
        return run_fmt(std::string{argv[2]}, out, err);
    }
    if (sub == "audit") {
        AuditOptions opts;
        for (std::size_t i = 2; i < argv.size(); ++i) {
            auto a = argv[i];
            if (a == "--sysreg") {
                opts.sysreg = true;
            } else if (a == "--no-libc") {
                opts.no_libc = true;
            } else if (!a.empty() && a[0] != '-') {
                opts.input = std::string{a};
            } else {
                err << "vestra: unknown audit option '" << a << "'\n";
                return 2;
            }
        }
        if (opts.input.empty()) {
            err << "vestra audit: missing input file\n";
            print_usage(err);
            return 2;
        }
        if (!opts.sysreg && !opts.no_libc) {
            err << "vestra audit: pick an enumerator (--sysreg, --no-libc)\n";
            return 2;
        }
        return run_audit(opts, out, err);
    }
    err << "vestra: unknown subcommand '" << sub << "'\n";
    print_usage(err);
    return 2;
}

int run_build(const BuildOptions& opts, std::ostream& out, std::ostream& err) {
    diag::SourceManager sm;
    diag::FileId fid;
    try {
        fid = sm.load_file(opts.input);
    } catch (const std::exception& ex) {
        err << "vestra: " << ex.what() << "\n";
        return 1;
    }

    diag::DiagnosticReporter rep(sm);
    lex::Lexer lex(sm, fid, rep);
    auto tokens = lex.tokenize();

    if (opts.dump_tokens) {
        for (const auto& t : tokens) {
            out << "  " << lex::spelling(t.kind) << "  '" << t.lexeme << "'\n";
        }
    }

    parse::Parser parser(tokens, rep);
    auto unit = parser.parse_unit();

    if (opts.dump_ast) {
        ast::Printer pr;
        pr.print_to(out, unit);
    }

    if (rep.has_errors()) {
        rep.render_to(err);
        return 1;
    }

    // The resolver's lifetime needs to outlive the emitter — the emitter
    // reads back the side tables (Resolution) to lower constructs that
    // depend on context, like leading-dot enum cases and match scrutinees.
    sema::TypeArena arena;
    // §12.1 `@embed`: the resolver/folder need a way to read files at
    // fold time. The reader resolves a path relative to the source
    // file's parent directory (a content-hashed manifest is the next
    // step; for now any relative path the user spells reads from disk).
    auto embed_base = opts.input.parent_path();
    sema::ComptimeFolder::EmbedReader embed_reader =
        [embed_base](std::string_view path) -> std::optional<std::vector<std::uint8_t>> {
        std::filesystem::path p{path};
        if (p.is_relative()) {
            p = embed_base / p;
        }
        std::ifstream f(p, std::ios::binary);
        if (!f) {
            return std::nullopt;
        }
        std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(f)),
                                        std::istreambuf_iterator<char>());
        return bytes;
    };
    // §A4 (§14.9.4) target-feature context flows from BuildOptions
    // into the resolver so the wide-atomic gate fires at the right
    // sites.
    sema::TargetContext target_ctx{opts.target, opts.target_features};
    sema::Resolver resolver(unit, arena, rep, std::move(embed_reader), std::move(target_ctx));
    if (!opts.skip_check) {
        resolver.resolve();
        if (rep.has_errors()) {
            rep.render_to(err);
            return 1;
        }
        sema::OwnershipChecker ownership(unit, resolver.resolution(), rep);
        ownership.check();
        sema::ExclusivityChecker exclusivity(unit, resolver.resolution(), rep);
        exclusivity.check();
        sema::CapabilityChecker capability(unit, resolver.resolution(), rep);
        capability.check();
        if (rep.has_errors()) {
            rep.render_to(err);
            return 1;
        }
    }

    codegen::CppEmitter emitter(rep, opts.skip_check ? nullptr : &resolver.resolution());
    emitter.set_no_libc(opts.no_libc);
    auto basename = opts.input.stem().string();
    auto em = emitter.emit(unit, basename);

    if (opts.emit_only) {
        out << "// === " << basename << ".hpp ===\n" << em.header << "\n";
        out << "// === " << basename << ".cpp ===\n" << em.source << "\n";
    } else {
        std::filesystem::create_directories(opts.out_dir);
        auto hpath = opts.out_dir / (basename + ".hpp");
        auto cpath = opts.out_dir / (basename + ".cpp");
        {
            std::ofstream h(hpath);
            h << em.header;
        }
        {
            std::ofstream c(cpath);
            c << em.source;
        }
        out << "wrote " << hpath << "\nwrote " << cpath << "\n";
    }

    rep.render_to(err);
    return rep.has_errors() ? 1 : 0;
}

int run_check(const std::filesystem::path& input, std::ostream& out, std::ostream& err) {
    diag::SourceManager sm;
    diag::FileId fid;
    try {
        fid = sm.load_file(input);
    } catch (const std::exception& ex) {
        err << "vestra: " << ex.what() << "\n";
        return 1;
    }
    diag::DiagnosticReporter rep(sm);
    lex::Lexer lex(sm, fid, rep);
    auto tokens = lex.tokenize();
    parse::Parser parser(tokens, rep);
    auto unit = parser.parse_unit();
    if (!rep.has_errors()) {
        sema::TypeArena arena;
        sema::Resolver resolver(unit, arena, rep);
        resolver.resolve();
        if (!rep.has_errors()) {
            sema::OwnershipChecker ownership(unit, resolver.resolution(), rep);
            ownership.check();
            sema::ExclusivityChecker exclusivity(unit, resolver.resolution(), rep);
            exclusivity.check();
            sema::CapabilityChecker capability(unit, resolver.resolution(), rep);
            capability.check();
        }
    }
    if (rep.has_errors()) {
        rep.render_to(err);
        return 1;
    }
    out << "vestra: " << input.string() << " checked OK\n";
    return 0;
}

int run_fmt(const std::filesystem::path& input, std::ostream& out, std::ostream& err) {
    diag::SourceManager sm;
    diag::FileId fid;
    try {
        fid = sm.load_file(input);
    } catch (const std::exception& ex) {
        err << "vestra: " << ex.what() << "\n";
        return 1;
    }
    diag::DiagnosticReporter rep(sm);
    lex::Lexer lex(sm, fid, rep);
    auto tokens = lex.tokenize();
    parse::Parser parser(tokens, rep);
    auto unit = parser.parse_unit();
    if (rep.has_errors()) {
        rep.render_to(err);
        return 1;
    }
    ast::Printer pr;
    pr.print_to(out, unit);
    return 0;
}

// §14.12.3 sysreg audit walker. Recursively walks the unit's decls
// looking for `Sysreg.<name>.{read|write}(...)` calls; for each site,
// prints `<file>:<line>:<col>: Sysreg.<name>.<op> [barrier:{yes|no}]`.
// The barrier flag fires when the name is in the gated set
// (sctlr_el1 / vbar_el1 / ttbr0_el1) AND the enclosing function isn't
// carrying @no_auto_barrier — same logic as the codegen-side
// dispatch.
namespace {

struct SysregAuditor {
    const diag::SourceManager& sm;
    std::ostream& out;
    bool in_no_auto_barrier = false;

    void walk_unit(const ast::CompilationUnit& unit) {
        for (const auto& d : unit.decls) {
            if (d != nullptr) {
                walk_decl(*d);
            }
        }
    }

    void walk_decl(const ast::Decl& d) {
        if (d.kind == ast::NodeKind::Func) {
            const auto& f = static_cast<const ast::FuncDecl&>(d);
            const bool prev = in_no_auto_barrier;
            for (const auto& a : f.attributes) {
                if (a.name == "no_auto_barrier") {
                    in_no_auto_barrier = true;
                    break;
                }
            }
            if (f.body != nullptr) {
                walk_expr(*f.body);
            }
            in_no_auto_barrier = prev;
        }
    }

    void walk_expr(const ast::Expr& e) {
        if (e.kind == ast::NodeKind::CallExpr) {
            const auto& c = static_cast<const ast::CallExpr&>(e);
            if (c.callee != nullptr && c.callee->kind == ast::NodeKind::MemberExpr) {
                const auto& mem = static_cast<const ast::MemberExpr&>(*c.callee);
                if ((mem.member == "read" || mem.member == "write") && mem.base != nullptr
                    && mem.base->kind == ast::NodeKind::MemberExpr) {
                    const auto& bm = static_cast<const ast::MemberExpr&>(*mem.base);
                    if (bm.base != nullptr && bm.base->kind == ast::NodeKind::IdentExpr) {
                        const auto& root = static_cast<const ast::IdentExpr&>(*bm.base);
                        if (root.name == "Sysreg") {
                            emit_record(e.range, bm.member, mem.member);
                        }
                    }
                }
            }
            if (c.callee) {
                walk_expr(*c.callee);
            }
            for (const auto& a : c.args) {
                if (a.value) {
                    walk_expr(*a.value);
                }
            }
            return;
        }
        if (e.kind == ast::NodeKind::BlockExpr) {
            const auto& b = static_cast<const ast::BlockExpr&>(e);
            for (const auto& s : b.stmts) {
                if (s != nullptr) {
                    walk_stmt(*s);
                }
            }
            return;
        }
        if (e.kind == ast::NodeKind::IfExpr) {
            const auto& i = static_cast<const ast::IfExpr&>(e);
            if (i.cond) {
                walk_expr(*i.cond);
            }
            if (i.then_branch) {
                walk_expr(*i.then_branch);
            }
            if (i.else_branch) {
                walk_expr(*i.else_branch);
            }
            return;
        }
        if (e.kind == ast::NodeKind::MemberExpr) {
            const auto& m = static_cast<const ast::MemberExpr&>(e);
            if (m.base) {
                walk_expr(*m.base);
            }
            return;
        }
        if (e.kind == ast::NodeKind::BinaryExpr) {
            const auto& b = static_cast<const ast::BinaryExpr&>(e);
            if (b.lhs) {
                walk_expr(*b.lhs);
            }
            if (b.rhs) {
                walk_expr(*b.rhs);
            }
            return;
        }
        // Other expression shapes (literals, idents, …) carry no nested
        // call sites we need to visit for the v0.5 sysreg audit.
    }

    void walk_stmt(const ast::Stmt& s) {
        switch (s.kind) {
        case ast::NodeKind::ExprStmt: {
            const auto& es = static_cast<const ast::ExprStmt&>(s);
            if (es.expr) {
                walk_expr(*es.expr);
            }
            break;
        }
        case ast::NodeKind::LetStmt: {
            const auto& l = static_cast<const ast::LetStmt&>(s);
            if (l.value) {
                walk_expr(*l.value);
            }
            break;
        }
        case ast::NodeKind::VarStmt: {
            const auto& v = static_cast<const ast::VarStmt&>(s);
            if (v.value) {
                walk_expr(*v.value);
            }
            break;
        }
        case ast::NodeKind::ReturnStmt: {
            const auto& r = static_cast<const ast::ReturnStmt&>(s);
            if (r.value) {
                walk_expr(*r.value);
            }
            break;
        }
        case ast::NodeKind::AssignStmt: {
            const auto& a = static_cast<const ast::AssignStmt&>(s);
            if (a.target) {
                walk_expr(*a.target);
            }
            if (a.value) {
                walk_expr(*a.value);
            }
            break;
        }
        case ast::NodeKind::WithStmt: {
            const auto& w = static_cast<const ast::WithStmt&>(s);
            for (const auto& b : w.bindings) {
                if (b.value) {
                    walk_expr(*b.value);
                }
            }
            if (w.body) {
                walk_expr(*w.body);
            }
            break;
        }
        default:
            break;
        }
    }

    void emit_record(diag::SourceRange range, std::string_view name, std::string_view op) {
        auto lc = sm.line_col(range.begin);
        auto path = sm.name(range.begin.file);
        // The gated set must match the codegen's table — these are the
        // sysregs that need an ISB on the trailing edge per §14.12.3.
        static const std::array<std::string_view, 6> gated = {
            "sctlr_el1",
            "vbar_el1",
            "ttbr0_el1",
            "ttbr1_el1",
            "tcr_el1",
            "mair_el1",
        };
        const bool is_gated_write =
            (op == "write") && std::find(gated.begin(), gated.end(), name) != gated.end();
        const bool barrier = is_gated_write && !in_no_auto_barrier;
        out << path << ":" << lc.line << ":" << lc.col << ": Sysreg." << name << "." << op
            << " [barrier:" << (barrier ? "yes" : "no") << "]\n";
    }
};

// §15.5 no_libc audit walker. Enumerates every `@extern + @symbol`
// declaration in the unit so a freestanding profile can prove no
// external symbol slipped in undeclared. Each finding emits one line
// `<file>:<line>:<col>: extern <symbol-name> [conv:<calling-conv>]`.
// A zero-finding pass under `profile.freestanding.no_libc = true` is
// the build-time proof §15.5 calls for.
struct NoLibcAuditor {
    const diag::SourceManager& sm;
    std::ostream& out;

    void walk_unit(const ast::CompilationUnit& unit) {
        for (const auto& d : unit.decls) {
            if (d == nullptr) {
                continue;
            }
            if (d->kind == ast::NodeKind::Func) {
                emit_for_attrs(d->range, static_cast<const ast::FuncDecl&>(*d).attributes);
            } else if (d->kind == ast::NodeKind::Static) {
                emit_for_attrs(d->range, static_cast<const ast::StaticDecl&>(*d).attributes);
            }
        }
    }

    void emit_for_attrs(diag::SourceRange decl_range, const std::vector<ast::Attribute>& attrs) {
        // The audit is interested in declarations that *create* an
        // external symbol entry: either an `@extern("conv")` that
        // names the calling convention or a `@symbol("name")` that
        // pins the link-time name. Either by itself produces an
        // external symbol; both together is the common kernel
        // shape (e.g., `@extern("C") @symbol("__stack_chk_guard")`).
        std::string_view symbol;
        std::string_view conv;
        diag::SourceRange site = decl_range;
        for (const auto& a : attrs) {
            if (a.name == "symbol" && a.predicate != nullptr
                && a.predicate->kind == ast::NodeKind::StringLit) {
                symbol = static_cast<const ast::StringLit&>(*a.predicate).text;
                site = a.range;
            } else if (a.name == "extern" && a.predicate != nullptr
                       && a.predicate->kind == ast::NodeKind::StringLit) {
                conv = static_cast<const ast::StringLit&>(*a.predicate).text;
                site = a.range;
            }
        }
        if (symbol.empty() && conv.empty()) {
            return;
        }
        auto lc = sm.line_col(site.begin);
        auto path = sm.name(site.begin.file);
        out << path << ":" << lc.line << ":" << lc.col << ": extern ";
        if (!symbol.empty()) {
            out << symbol;
        } else {
            out << "(unnamed)";
        }
        if (!conv.empty()) {
            out << " [conv:" << conv << "]";
        }
        out << "\n";
    }
};

}  // namespace

int run_audit(const AuditOptions& opts, std::ostream& out, std::ostream& err) {
    diag::SourceManager sm;
    diag::FileId fid;
    try {
        fid = sm.load_file(opts.input);
    } catch (const std::exception& ex) {
        err << "vestra: " << ex.what() << "\n";
        return 1;
    }
    diag::DiagnosticReporter rep(sm);
    lex::Lexer lex(sm, fid, rep);
    auto tokens = lex.tokenize();
    parse::Parser parser(tokens, rep);
    auto unit = parser.parse_unit();
    if (rep.has_errors()) {
        rep.render_to(err);
        return 1;
    }
    if (opts.sysreg) {
        SysregAuditor a{sm, out};
        a.walk_unit(unit);
    }
    if (opts.no_libc) {
        NoLibcAuditor a{sm, out};
        a.walk_unit(unit);
    }
    return 0;
}

}  // namespace vestra::driver
