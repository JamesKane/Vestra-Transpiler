// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#pragma once

// §12.4 AST deep-clone. Produces an owned copy of an AST node and everything
// it references, preserving source ranges. Used to materialize macro
// templates (a declaration macro's `quote { … }` body) into fresh
// declarations so one macro can back several expansion sites, and as the
// foundation the comptime AST-value model builds on. Covers the node kinds a
// v0.5 quote template uses; an unsupported kind yields a `<unsupported>`
// placeholder node rather than silently corrupting the tree.

#include "vestra/ast/nodes.hpp"

namespace vestra::ast {

[[nodiscard]] ExprPtr clone(const Expr& e);
[[nodiscard]] StmtPtr clone(const Stmt& s);
[[nodiscard]] DeclPtr clone(const Decl& d);
[[nodiscard]] TypePtr clone(const Type& t);
[[nodiscard]] PatternPtr clone(const Pattern& p);

}  // namespace vestra::ast
