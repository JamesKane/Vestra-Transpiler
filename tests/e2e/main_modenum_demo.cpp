// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

// Driver for examples/modenum_demo (F19: imported enums and struct fields).
#include "main.hpp"

#include <cassert>
#include <cstdio>

int main() {
    assert(demo::kern::payloadOf(demo::kern::qualified()) == 0);   // kern.ipc.Held.client(0)
    assert(demo::kern::payloadOf(demo::kern::contextual()) == 1);  // .client(1) against kern.ipc.Held
    assert(demo::kern::bareCases());                                // .none / Held.none / No cases
    assert(demo::kern::caught(5) == 5);
    assert(demo::kern::caught(-1) == 1001);                         // catch (e: kern.ipc.No)
    assert(demo::kern::nested());                                   // k.domains[i].held across modules
    assert(demo::kern::freshAddress() == reinterpret_cast<std::uintptr_t>(&kern::ipc::fresh));
    std::puts("modenum_demo OK");
    return 0;
}
