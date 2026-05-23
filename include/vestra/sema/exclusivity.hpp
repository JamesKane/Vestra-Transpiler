#pragma once

#include "vestra/ast/nodes.hpp"
#include "vestra/diag/diagnostic.hpp"
#include "vestra/diag/source_manager.hpp"
#include "vestra/sema/resolver.hpp"
#include "vestra/sema/scope.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace vestra::sema {

// The Law of Exclusivity — §5.4 and §19.3 — phase 1.
//
// At any program point, the live accesses to a storage location must be
// either all `read` (shared) or exactly one `inout` (exclusive). Phase 1
// enforces this per-call: within a single call's argument list, no two
// arguments may produce overlapping accesses that conflict.
//
// Place model
//   A Place names a (storage location) as a root Symbol* plus a path of
//   field selectors. `x` is ([Symbol(x)], []). `p.x.y` is ([Symbol(p)],
//   [".x", ".y"]). Two places overlap iff they share a root AND one path
//   is a prefix of the other. Clause 3 (disjoint fields are independent)
//   falls out of this for free: `&p.x` and `&p.y` have the same root but
//   neither path is a prefix of the other.
//
// What this pass deliberately does not (yet) do
//   * Cross-statement liveness — borrows are only tracked within one call.
//     A binding held inout across two statements isn't flagged yet.
//   * Partition primitives (clause 4) — `chunks(n)` / `split(at:)` /
//     compositions thereof aren't recognized as disjoint partitions.
//   * Index-based subviews — `&buf[i]` vs `&buf[j]` is approximated as
//     overlapping (conservative; rejects safe code).
//   * Closures and async — borrows escaping into a closure aren't tracked.
class ExclusivityChecker {
public:
    ExclusivityChecker(const ast::CompilationUnit& unit,
                       const Resolution& resolution,
                       diag::DiagnosticReporter& reporter)
        : unit_(&unit), resolution_(&resolution), reporter_(&reporter) {}

    ExclusivityChecker(const ExclusivityChecker&) = delete;
    ExclusivityChecker& operator=(const ExclusivityChecker&) = delete;

    void check();

private:
    // NOLINTNEXTLINE(performance-enum-size)
    enum class Access : std::uint8_t { Read, Inout };

    struct Place {
        const Symbol* root = nullptr;
        std::vector<std::string> path;  // field selectors, in outermost-first order
    };

    struct Borrow {
        Place place;
        Access access = Access::Read;
        diag::SourceRange site{};
    };

    void check_decl(const ast::Decl& d);
    void check_func(const ast::FuncDecl& f);
    void check_stmt(const ast::Stmt& s);
    void check_expr(const ast::Expr& e);
    void check_call(const ast::CallExpr& c);

    // If `e` is a place expression (identifier or `place.field` chain),
    // return its Place; otherwise std::nullopt. The root must be a
    // trackable Symbol (Local or Parameter); callers handle the case where
    // it isn't (e.g., calls into a global Const).
    [[nodiscard]] std::optional<Place> as_place(const ast::Expr& e) const;

    [[nodiscard]] static bool overlap(const Place& a, const Place& b) noexcept;
    [[nodiscard]] static bool conflict(Access a, Access b) noexcept;

    [[nodiscard]] static std::string render_place(const Place& p);

    const ast::CompilationUnit* unit_;
    const Resolution* resolution_;
    diag::DiagnosticReporter* reporter_;
};

}  // namespace vestra::sema
