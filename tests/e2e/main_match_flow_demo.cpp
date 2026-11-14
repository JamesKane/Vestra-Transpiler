// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

// Driver for examples/match_flow_demo (F18).
#include "match_flow_demo.hpp"

#include <cassert>
#include <cstdio>

int main() {
    using namespace examples::match_flow_demo;
    assert(endpointOf(mk(0)) == 5u && endpointOf(mk(1)) == 9u && !endpointOf(mk(2)).has_value());
    assert(isClient(mk(0)) && !isClient(mk(1)));
    assert(servedOr(mk(1), 40) == 9 && servedOr(mk(0), 40) == 41 && servedOr(mk(2), 40) == 40);
    assert(stopAt(Light::red, false) == 0 && stopAt(Light::amber, true) == 1);
    assert(stopAt(Light::amber, false) == 2 && stopAt(Light::green, true) == 2);
    assert(firstOver(3) == 4);   // returns before i reaches 7
    assert(firstOver(50) == 7);  // the arm's break leaves the loop at 7
    assert(drain(4) == 3 + 2 + 1 + 0);
    std::puts("match_flow_demo OK");
    return 0;
}
