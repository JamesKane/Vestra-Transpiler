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

    // §12.4 declaration macro: @withOrigin kept `struct Marker` ($d), added a
    // companion `origin()`, and a `markerName()` reflecting the struct's name
    // via `$(d.name)`.
    qd::Marker mk{.tag = 7};
    assert(mk.tag == 7);
    assert(qd::origin() == 0);
    assert(qd::markerName() == "Marker");

    // §12.4 declaration macro through the comptime folder: @bump kept `struct
    // Counter` ($d) and added `answer()`, whose body is the *computed* splice
    // `$(k)` where `k = 40 + 2` was folded to 42 during expansion.
    qd::Counter ct{.n = 3};
    assert(ct.n == 3);
    assert(qd::answer() == 42);

    // §12.4 field reflection: @described iterated `d.fields` at comptime,
    // counting all three fields and the two Int64 ones (via f.type.name), and
    // spliced the folded totals into the generated accessors.
    qd::Vec3 v3{.x = 1, .y = 2, .z = 3};
    assert(v3.x == 1);
    assert(qd::fieldCount() == 3);
    assert(qd::wideFieldCount() == 2);

    // §12.4 per-field decl generation: @labels kept `struct Pair` ($d) and
    // appended one accessor per field via `[Decl] +=`, each named by a comptime
    // string splice (`first_label`, `second_label`) returning the field name.
    qd::Pair pr{.first = 10, .second = 20};
    assert(pr.first == 10);
    assert(qd::first_label() == "first");
    assert(qd::second_label() == "second");

    std::puts("quote_demo OK");
    return 0;
}
