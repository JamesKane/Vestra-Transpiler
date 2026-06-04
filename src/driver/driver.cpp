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
#include <format>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <ostream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace vestra::driver {

namespace {

constexpr std::string_view UsageText = R"(usage: vestra <subcommand> [options]

Subcommands:
  build <file.vst> [-o DIR] [-I DIR ...] [--emit-only] [--dump-ast]
                   [--dump-tokens] [--skip-check] [--no-libc]
                   [--target=ARCH] [--target-features=LIST]
        Parse, semantically check, then transpile a Vestra source file to
        C++ (.hpp + .cpp) under DIR. §5: `import a.b.c` is followed to
        <entry-dir>/a/b/c.vst, and every transitively-imported module is
        transpiled to its own .hpp/.cpp under DIR.
        -I DIR adds an import search root (repeatable): an import resolves
        against the entry file's directory first, then each -I root in
        order, taking the first match.
        --skip-check elides sema (debug aid).
        --no-libc marks the build as freestanding (§A10 §15.5): the
        generated header opens with `// vestra: no_libc = true`.
        --target picks the target architecture (host / aarch64 / x86_64 /
        riscv64; default host). --target-features takes a comma-list of
        ISA features (e.g. lse2,cx16). Wide atomics (Atomic[UInt128] /
        AtomicTaggedPointer[T]) require the matching feature when the
        target is non-host: lse2 on aarch64, cx16 on x86_64 (§14.9.4).
  check <file.vst> [-I DIR ...]
        Parse and run name resolution + type checking; print any diagnostics.
        Exits 0 on a clean check. -I DIR adds an import search root (as in
        build), so `check` resolves imports identically.
  fmt <file.vst>
        Pretty-print a Vestra source file to stdout.
  expand <file.vst>
        Pretty-print a Vestra source file after §12.4 declaration-macro
        expansion — shows the declarations each `@macro` generated (the
        comptime macro definitions themselves drop out). Runs before sema.
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
            } else if (a == "-I" && i + 1 < argv.size()) {
                opts.import_paths.emplace_back(std::string{argv[++i]});
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
        std::filesystem::path input;
        std::vector<std::filesystem::path> import_paths;
        for (std::size_t i = 2; i < argv.size(); ++i) {
            auto a = argv[i];
            if (a == "-I" && i + 1 < argv.size()) {
                import_paths.emplace_back(std::string{argv[++i]});
            } else if (!a.empty() && a[0] != '-') {
                input = std::string{a};
            } else {
                err << "vestra: unknown option '" << a << "'\n";
                return 2;
            }
        }
        if (input.empty()) {
            err << "vestra check: missing input file\n";
            return 2;
        }
        return run_check(input, out, err, import_paths);
    }
    if (sub == "fmt") {
        if (argv.size() < 3) {
            err << "vestra fmt: missing input file\n";
            return 2;
        }
        return run_fmt(std::string{argv[2]}, out, err);
    }
    if (sub == "expand") {
        if (argv.size() < 3) {
            err << "vestra expand: missing input file\n";
            return 2;
        }
        return run_expand(std::string{argv[2]}, out, err);
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

namespace {

// §12.1 `@embed` reader rooted at `base`: resolves a relative path against the
// source file's directory and returns its bytes (nullopt on any failure).
sema::ComptimeFolder::EmbedReader make_embed_reader(std::filesystem::path base) {
    return [base = std::move(base)](
               std::string_view path) -> std::optional<std::vector<std::uint8_t>> {
        std::filesystem::path p{path};
        if (p.is_relative()) {
            p = base / p;
        }
        std::ifstream f(p, std::ios::binary);
        if (!f) {
            return std::nullopt;
        }
        return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(f)),
                                         std::istreambuf_iterator<char>());
    };
}

// §5 the parsed module graph for an entry file: the entry and every
// transitively-imported unit (owned, heap-stable), each unit's source path, and
// each unit's direct-import indices into `units`. `entry` indexes the root.
struct ModuleGraph {
    std::vector<std::unique_ptr<ast::CompilationUnit>> units;
    std::vector<std::filesystem::path> files;
    std::vector<std::vector<int>> deps;
    // §5 output path for each unit relative to the -o directory (no extension):
    // the entry keeps its file stem; a dependency uses its import path as a
    // directory path (`import util.math` → "util/math"), so same-named modules
    // in different packages don't collide and the importer's `#include` (which
    // mirrors the import path) finds it.
    std::vector<std::string> relpaths;
    int entry = -1;
};

// Load `input` and follow `import a.b.c` to <entry-dir>/a/b/c.vst transitively,
// deduping by canonical path (a diamond loads once). Parse diagnostics go to
// `rep`; so do located §5 import errors — a missing imported file and a cyclic
// import each report at the offending `import` statement, so the build aborts
// (via `rep.has_errors()`) before resolution. A failure to load the *entry*
// itself is reported to `err` and yields a graph whose `entry` is -1. Used by
// both `build` and `check` so they agree on which modules are in play.
ModuleGraph load_module_graph(const std::filesystem::path& input,
                              diag::SourceManager& sm,
                              diag::DiagnosticReporter& rep,
                              std::ostream& err,
                              const std::vector<std::filesystem::path>& import_roots = {}) {
    namespace fs = std::filesystem;
    // §5 search roots, in priority order: the entry file's directory first,
    // then each `-I` root. An import resolves to the first existing candidate;
    // its output relpath stays the import path regardless of which root held it.
    std::vector<fs::path> search_dirs;
    search_dirs.push_back(input.parent_path());
    for (const auto& r : import_roots) {
        search_dirs.push_back(r);
    }
    ModuleGraph g;
    std::unordered_map<std::string, int> visited;
    // Canonical keys of the units currently on the DFS recursion stack. An
    // `import` whose target is already on this stack closes a cycle (a
    // back-edge); a target merely in `visited` but off the stack is a diamond
    // re-import and is fine.
    std::unordered_set<std::string> on_stack;

    auto canon_key = [](const fs::path& p) -> std::string {
        std::error_code ec;
        auto c = fs::weakly_canonical(p, ec);
        return (ec ? p : c).string();
    };

    std::function<int(const fs::path&, std::string)> load = [&](const fs::path& file,
                                                                std::string relpath) -> int {
        std::string key = canon_key(file);
        if (auto it = visited.find(key); it != visited.end()) {
            return it->second;
        }
        diag::FileId fid;
        try {
            fid = sm.load_file(file);
        } catch (const std::exception& e) {
            err << "vestra: " << e.what() << "\n";
            return -1;
        }
        lex::Lexer lx(sm, fid, rep);
        auto toks = lx.tokenize();
        parse::Parser ps(toks, rep);
        const int idx = static_cast<int>(g.units.size());
        g.units.push_back(std::make_unique<ast::CompilationUnit>(ps.parse_unit()));
        g.files.push_back(file);
        g.relpaths.push_back(std::move(relpath));
        g.deps.emplace_back();
        visited[key] = idx;
        on_stack.insert(key);
        // Resolve each `import` to a file and recurse. The unit object is
        // heap-stable, so caching its pointer across the recursion (which may
        // reallocate `g.units`) is safe.
        auto* up = g.units[static_cast<std::size_t>(idx)].get();
        for (const auto& imp : up->imports) {
            if (imp == nullptr || imp->is_c_header || imp->path.empty()) {
                continue;
            }
            // Build the import as a relative directory path (`a.b.c` →
            // `a/b/c.vst`) and the dotted name for diagnostics.
            fs::path rel;
            std::string dep_rel;
            std::string dotted;
            for (const auto& seg : imp->path) {
                rel /= seg;
                if (!dep_rel.empty()) {
                    dep_rel += '/';
                }
                dep_rel += seg;
                if (!dotted.empty()) {
                    dotted += '.';
                }
                dotted += seg;
            }
            rel += ".vst";
            // §5 resolve against each search root in order; the first existing
            // candidate wins. The relpath that drives output layout stays
            // `dep_rel` (the import path), independent of which root matched.
            fs::path dep;
            bool found = false;
            for (const auto& base : search_dirs) {
                std::error_code ec;
                fs::path cand = base / rel;
                if (fs::exists(cand, ec) && !ec) {
                    dep = std::move(cand);
                    found = true;
                    break;
                }
            }
            // §5 missing-import diagnostic, located at the `import` statement;
            // names every root searched so a misconfigured `-I` is obvious.
            if (!found) {
                std::string searched;
                for (const auto& base : search_dirs) {
                    if (!searched.empty()) {
                        searched += ", ";
                    }
                    searched += (base / rel).string();
                }
                rep.report(diag::Diagnostic::error(
                               std::format("cannot find imported module '{}' (searched: {})",
                                           dotted,
                                           searched))
                               .at(imp->range));
                continue;
            }
            // §5 cyclic-import diagnostic: the target is still on the DFS stack,
            // so this `import` closes a cycle. Report and drop the back-edge
            // (don't recurse), keeping the dependency graph acyclic for topo.
            if (on_stack.contains(canon_key(dep))) {
                rep.report(diag::Diagnostic::error(
                               std::format("cyclic import: '{}' is already being imported "
                                           "(import cycle)",
                                           dotted))
                               .at(imp->range));
                continue;
            }
            const int di = load(dep, dep_rel);
            if (di >= 0) {
                g.deps[static_cast<std::size_t>(idx)].push_back(di);
            }
        }
        on_stack.erase(key);
        return idx;
    };

    // The entry keeps its file stem as its output name (so single-file builds
    // are unchanged); dependencies get an import-path-derived relpath above.
    g.entry = load(input, input.stem().string());
    return g;
}

// A unit's dotted `module` path ("util.math"), or empty if it declares none.
std::string module_path_of(const ast::CompilationUnit& u) {
    if (u.module == nullptr) {
        return {};
    }
    std::string dotted;
    for (const auto& seg : u.module->path) {
        if (!dotted.empty()) {
            dotted += '.';
        }
        dotted += seg;
    }
    return dotted;
}

// A unit's C++ namespace ("util::math"), or empty if it declares no module.
std::string module_namespace_of(const ast::CompilationUnit& u) {
    if (u.module == nullptr) {
        return {};
    }
    std::string ns;
    for (const auto& seg : u.module->path) {
        if (!ns.empty()) {
            ns += "::";
        }
        ns += seg;
    }
    return ns;
}

// §5 — build the importer-side map the emitter uses to fully-qualify a
// computed/inferred imported nominal: every top-level struct/enum/protocol/
// opaque decl of every *other* module unit, keyed by its decl pointer (the
// same pointer a sema type's `nominal_decl()` carries), mapped to that unit's
// C++ namespace. The current unit `self` is excluded so its own types stay
// bare; module-less units (global scope) contribute nothing.
std::unordered_map<const ast::Decl*, std::string> imported_qualifiers_for(const ModuleGraph& g,
                                                                          std::size_t self) {
    std::unordered_map<const ast::Decl*, std::string> q;
    for (std::size_t j = 0; j < g.units.size(); ++j) {
        if (j == self) {
            continue;
        }
        std::string ns = module_namespace_of(*g.units[j]);
        if (ns.empty()) {
            continue;
        }
        for (const auto& d : g.units[j]->decls) {
            switch (d->kind) {
            case ast::NodeKind::Struct:
            case ast::NodeKind::Enum:
            case ast::NodeKind::Protocol:
            case ast::NodeKind::Opaque:
                q[d.get()] = ns;
                break;
            default:
                break;
            }
        }
    }
    return q;
}

// Dependency-first ordering of the units in `g` (post-order DFS over deps), so
// a module is resolved — and its exports harvested — before any importer.
std::vector<std::size_t> topo_order(const ModuleGraph& g) {
    std::vector<std::size_t> order;
    std::vector<bool> seen(g.units.size(), false);
    std::function<void(std::size_t)> visit = [&](std::size_t i) {
        if (seen[i]) {
            return;
        }
        seen[i] = true;
        for (int d : g.deps[i]) {
            visit(static_cast<std::size_t>(d));
        }
        order.push_back(i);
    };
    for (std::size_t i = 0; i < g.units.size(); ++i) {
        visit(i);
    }
    return order;
}

}  // namespace

int run_build(const BuildOptions& opts, std::ostream& out, std::ostream& err) {
    diag::SourceManager sm;
    diag::DiagnosticReporter rep(sm);
    namespace fs = std::filesystem;

    // §5 multi-file: load the entry and every transitively-imported module,
    // resolving imports against the entry dir then any `-I` roots.
    ModuleGraph graph = load_module_graph(opts.input, sm, rep, err, opts.import_paths);
    if (graph.entry < 0) {
        return 1;
    }
    auto& units = graph.units;
    auto& files = graph.files;
    auto& deps = graph.deps;
    auto& relpaths = graph.relpaths;
    const int entry = graph.entry;
    if (opts.dump_tokens) {
        diag::FileId fid = sm.load_file(opts.input);
        for (const auto& t : lex::Lexer(sm, fid, rep).tokenize()) {
            out << "  " << lex::spelling(t.kind) << "  '" << t.lexeme << "'\n";
        }
    }

    // §12.4 / §5: expand declaration macros and fold extensions on every unit
    // before any resolution (so an importer collecting a dependency's decls
    // sees them in fully-lowered form).
    for (auto& u : units) {
        sema::expand_declaration_macros(*u, rep);
        sema::fold_extensions(*u);
    }
    if (opts.dump_ast) {
        ast::Printer pr;
        pr.print_to(out, *units[static_cast<std::size_t>(entry)]);
    }
    if (rep.has_errors()) {
        rep.render_to(err);
        return 1;
    }

    const sema::TargetContext target_ctx{opts.target, opts.target_features};
    if (!opts.emit_only) {
        fs::create_directories(opts.out_dir);
    }

    // §5 one TypeArena for the whole build, so a type computed while resolving a
    // dependency is valid when an importer reuses its exported symbols. Resolve
    // dependency-first; harvest each module's public exports into `store` for
    // its importers to reference (no signature re-derivation).
    sema::TypeArena arena;
    sema::Resolver::ModuleExports store;
    for (std::size_t i : topo_order(graph)) {
        auto& unit = *units[i];
        sema::Resolver resolver(
            unit, arena, rep, make_embed_reader(files[i].parent_path()), target_ctx);
        sema::Resolver::ModuleExports visible;
        for (int d : deps[i]) {
            const std::string dep_mod = module_path_of(*units[static_cast<std::size_t>(d)]);
            if (auto it = store.find(dep_mod); it != store.end()) {
                visible[dep_mod] = it->second;
            }
        }
        resolver.set_module_exports(std::move(visible));
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
            // Harvest this module's public exports for its importers (later in
            // topo order). Symbols are copied; their types live in the shared
            // arena, which outlives the build.
            if (auto mod = module_path_of(unit); !mod.empty()) {
                store[mod] = resolver.public_exports();
            }
        }

        codegen::CppEmitter emitter(rep, opts.skip_check ? nullptr : &resolver.resolution());
        emitter.set_no_libc(opts.no_libc);
        emitter.set_imported_qualifiers(imported_qualifiers_for(graph, i));
        // The output relpath (entry = stem; dependency = import path) is also the
        // emitter's `output_basename`, so a unit's `#include "<self>.hpp"` and an
        // importer's `#include "<dep>.hpp"` both spell the same -I-relative path.
        const auto& basename = relpaths[i];
        auto em = emitter.emit(unit, basename);
        if (opts.emit_only) {
            out << "// === " << basename << ".hpp ===\n" << em.header << "\n";
            out << "// === " << basename << ".cpp ===\n" << em.source << "\n";
        } else {
            auto hpath = opts.out_dir / (basename + ".hpp");
            auto cpath = opts.out_dir / (basename + ".cpp");
            fs::create_directories(hpath.parent_path());
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
    }

    rep.render_to(err);
    return rep.has_errors() ? 1 : 0;
}

int run_check(const std::filesystem::path& input,
              std::ostream& out,
              std::ostream& err,
              const std::vector<std::filesystem::path>& import_paths) {
    diag::SourceManager sm;
    diag::DiagnosticReporter rep(sm);

    // §5 multi-file: load + check the entry and every imported module, so
    // `check` agrees with `build` on which cross-module references resolve.
    ModuleGraph graph = load_module_graph(input, sm, rep, err, import_paths);
    if (graph.entry < 0) {
        return 1;
    }
    for (auto& u : graph.units) {
        sema::expand_declaration_macros(*u, rep);  // §12.4
        sema::fold_extensions(*u);                 // §5
    }
    if (rep.has_errors()) {
        rep.render_to(err);
        return 1;
    }
    // §5 dependency-first, one shared arena, harvest exports for importers —
    // mirrors run_build so `check` and `build` resolve identically.
    sema::TypeArena arena;
    sema::Resolver::ModuleExports store;
    for (std::size_t i : topo_order(graph)) {
        auto& unit = *graph.units[i];
        sema::Resolver resolver(unit, arena, rep, make_embed_reader(graph.files[i].parent_path()));
        sema::Resolver::ModuleExports visible;
        for (int d : graph.deps[i]) {
            const std::string dep_mod = module_path_of(*graph.units[static_cast<std::size_t>(d)]);
            if (auto it = store.find(dep_mod); it != store.end()) {
                visible[dep_mod] = it->second;
            }
        }
        resolver.set_module_exports(std::move(visible));
        resolver.resolve();
        if (!rep.has_errors()) {
            sema::OwnershipChecker ownership(unit, resolver.resolution(), rep);
            ownership.check();
            sema::ExclusivityChecker exclusivity(unit, resolver.resolution(), rep);
            exclusivity.check();
            sema::CapabilityChecker capability(unit, resolver.resolution(), rep);
            capability.check();
        }
        if (rep.has_errors()) {
            rep.render_to(err);
            return 1;
        }
        if (auto mod = module_path_of(unit); !mod.empty()) {
            store[mod] = resolver.public_exports();
        }
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

// §12.4 `vestra expand` — pretty-print a source file *after* declaration-macro
// expansion, so a macro author can see exactly what `@name` annotations
// generated (the macro definitions themselves are comptime-only and drop out).
// Like `fmt`, but with the expansion pass in between; runs before sema so the
// output is the program the resolver would then check.
int run_expand(const std::filesystem::path& input, std::ostream& out, std::ostream& err) {
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
    sema::expand_declaration_macros(unit, rep);  // §12.4
    sema::fold_extensions(unit);                 // §5 — show the lowered form
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
