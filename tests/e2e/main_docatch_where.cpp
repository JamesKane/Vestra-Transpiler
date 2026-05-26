// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#include "docatch_where.hpp"

#include <cstdlib>
#include <print>

int main() {
    using namespace examples::docatch_where;

    // Success path: makeReq(0) returns 42, so the do-body's expected
    // is a value and attempt's expected wraps it.
    if (auto r = attempt(0); !r.has_value() || *r != 42) {
        std::println("kind=0 should succeed with 42");
        return EXIT_FAILURE;
    }
    // Retryable family: the where-guard fires, the handler returns
    // -1, and attempt's expected wraps it.
    if (auto r = attempt(1); !r.has_value() || *r != -1) {
        std::println("kind=1 (timeout) should hit the retry arm");
        return EXIT_FAILURE;
    }
    if (auto r = attempt(2); !r.has_value() || *r != -1) {
        std::println("kind=2 (refused) should hit the retry arm");
        return EXIT_FAILURE;
    }
    // Fatal: the where-guard returns false, the bound NetErr.fatal
    // falls through and propagates as std::unexpected through
    // attempt's throws(NetErr) clause.
    if (auto r = attempt(3); r.has_value() || r.error() != NetErr::fatal) {
        std::println("kind=3 (fatal) should propagate NetErr.fatal");
        return EXIT_FAILURE;
    }

    std::println("docatch where OK");
    return EXIT_SUCCESS;
}
