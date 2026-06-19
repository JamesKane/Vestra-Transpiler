// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

// E2E driver for §18.5 HashMap iteration. Builds word-count maps and walks them
// via keys() / values() / entries() (the last with `for (k, v)` destructuring).
// Iteration order is unspecified, so every assertion is an order-independent
// aggregate.

#include "hashmap_iter_demo.hpp"

#include <cassert>
#include <cstdio>
#include <string_view>

namespace hm = examples::hashmap_iter_demo;

int main() {
    using std::string_view;

    // "a,bb,a,ccc,bb,a" -> counts a=3, bb=2, ccc=1.
    const auto in = string_view("a,bb,a,ccc,bb,a");

    // keys() length = distinct words.
    assert(hm::distinctCount(in) == 3);
    assert(hm::distinctCount(string_view("solo")) == 1);
    assert(hm::distinctCount(string_view("x,x,x,x")) == 1);

    // values() sum = total words.
    assert(hm::totalCount(in) == 6);
    assert(hm::totalCount(string_view("one,two,three")) == 3);
    assert(hm::totalCount(string_view("dup,dup")) == 2);

    // keys() length sum = sum of distinct-key byte lengths: 1 + 2 + 3 = 6.
    assert(hm::totalKeyLen(in) == 6);
    assert(hm::totalKeyLen(string_view("ab,cd,ab")) == 4);  // "ab"(2) + "cd"(2)

    // entries() destructured: sum of len(key)*count = 1*3 + 2*2 + 3*1 = 10.
    assert(hm::weightedSum(in) == 10);
    assert(hm::weightedSum(string_view("zz,zz,zz")) == 6);  // len 2 * count 3

    // entries() destructured: the largest single count.
    assert(hm::maxCount(in) == 3);                    // a appears 3x
    assert(hm::maxCount(string_view("p,q,r")) == 1);  // all distinct
    assert(hm::maxCount(string_view("k,k,k,k,k")) == 5);

    std::puts("hashmap_iter_demo OK");
    return 0;
}
