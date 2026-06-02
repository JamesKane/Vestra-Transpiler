// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

// E2E driver for §11 blocking channel receive on the cooperative scheduler.
// Drives each async entry point from a non-async (C++) caller via Task::get(),
// which pumps the run-loop to completion.

#include "channel_select_demo.hpp"

#include <cassert>
#include <cstdio>

namespace cs = examples::channel_select_demo;

int main() {
    // A consumer parks on the empty channel; the producer wakes it per send.
    // 1 + 2 + 3 = 6.
    assert(cs::pipeline().get() == 6);

    // close() wakes a parked receiver; a blocking receive on a drained closed
    // channel yields nil → -1 (rather than hanging the run-loop).
    assert(cs::closeWakes().get() == -1);

    // A no-default select parks on both channels; only `b` is fed, so the
    // parked select wakes on the `b` arm and binds 22.
    assert(cs::selectPicks().get() == 22);

    std::puts("channel_select_demo OK");
    return 0;
}
