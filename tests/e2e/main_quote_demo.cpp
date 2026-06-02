// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

// E2E driver for the §12.4 quote/splice first slice (expression context):
// confirms the quoted bodies materialize with their splices substituted
// and compute the right values.

#include "quote_demo.hpp"

#include <cassert>
#include <cstdio>

namespace qd = examples::quote_demo;

int main() {
    assert(qd::square(6) == 36);      // (x) * (x)
    assert(qd::axpy(2, 3, 4) == 10);  // a*x + y
    assert(qd::clampLow(1, 5) == 5);  // 1 < 5 -> lo
    assert(qd::clampLow(9, 5) == 9);  // 9 < 5 false -> v

    // §12.4 expression macros: @twice(n) -> n + n; @scaleBias(p+1, k) ->
    // (p+1)*(p+1) + k, the splice parens preserving precedence.
    assert(qd::useTwice(21) == 42);        // 21 + 21
    assert(qd::useScaleBias(2, 5) == 14);  // (2+1)*(2+1) + 5

    // §12.4 declaration macro: @withOrigin kept `struct Marker` ($d) and added
    // a companion `origin()`.
    qd::Marker mk{.tag = 7};
    assert(mk.tag == 7);
    assert(qd::origin() == 0);

    std::puts("quote_demo OK");
    return 0;
}
