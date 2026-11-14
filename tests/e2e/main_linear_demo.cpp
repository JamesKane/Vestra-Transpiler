// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

// Driver for examples/linear_demo (§19.6 linear handles in storage, F17).
// `put` into an occupied slot panics; `occupied` runs that path.
#include "linear_demo.hpp"

#include <cassert>
#include <cstdio>
#include <cstring>

int main(int argc, char** argv) {
    using namespace examples::linear_demo;
    auto t = fresh();
    assert(store(t, mint(7)) == 0);
    assert(store(t, mint(8)) == 1);
    storeAt(t, 3, mint(9));
    assert(idAt(t, 0) == 7 && idAt(t, 1) == 8 && idAt(t, 3) == 9);
    moveSlot(t, 0, 2);
    assert(idAt(t, 0) == 0 && idAt(t, 2) == 7);
    assert(store(t, mint(10)) == 0);  // the slot moveSlot emptied
    assert(release(t, 1) && !release(t, 1));
    if (argc > 1 && std::strcmp(argv[1], "occupied") == 0) {
        storeAt(t, 3, mint(11));  // slot 3 holds 9: panics
        return 0;
    }
    std::puts("linear_demo OK");
    return 0;
}
