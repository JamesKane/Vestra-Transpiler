#include "vestra/driver/driver.hpp"

#include "vestra/ast/printer.hpp"
#include "vestra/codegen/cpp_emitter.hpp"
#include "vestra/diag/diagnostic.hpp"
#include "vestra/diag/source_manager.hpp"
#include "vestra/lex/lexer.hpp"
#include "vestra/lex/token.hpp"
#include "vestra/parse/parser.hpp"
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
                   [--skip-check]
        Parse, semantically check, then transpile a Vestra source file to
        C++ (.hpp + .cpp) under DIR. --skip-check elides sema (debug aid).
  check <file.vst>
        Parse and run name resolution + type checking; print any diagnostics.
        Exits 0 on a clean check.
  fmt <file.vst>
        Pretty-print a Vestra source file to stdout.
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

    if (!opts.skip_check) {
        sema::TypeArena arena;
        sema::Resolver resolver(unit, arena, rep);
        resolver.resolve();
        if (rep.has_errors()) {
            rep.render_to(err);
            return 1;
        }
    }

    codegen::CppEmitter emitter(rep);
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

}  // namespace vestra::driver
