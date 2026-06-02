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

    // A timeout arm fires when no channel delivers: nothing feeds the channel,
    // so once the run-loop goes idle the 1ms wall-clock timer wins → -9.
    assert(cs::selectTimes().get() == -9);

    // A channel that delivers before the deadline beats the timeout: `b` is fed
    // before the run-loop goes idle, so the `b` arm wins over the 100ms timeout.
    assert(cs::selectBeatsTimeout().get() == 22);

    // Duration is Swift-like: `.seconds(n)` factories, and `Duration / Duration`
    // is a dimensionless Float64 ratio. 10s / 2s = 5.0; (500+1500)ms / 1s = 2.0.
    assert(cs::durationRatio() == 5.0);
    assert(cs::durationSum() == 2.0);

    // Scalar scaling: Duration * Int / Int * Duration / Duration / Int all
    // yield a Duration; 1000ms scaled to 2000ms, divided by 250ms = 8.0.
    assert(cs::durationScaled() == 8.0);

    // Accessors read the total whole count in a unit as Int (truncating):
    // 2s == 2000ms; 1500ms truncates to 1 whole second.
    assert(cs::durationMillis() == 2000);
    assert(cs::durationSecondsTrunc() == 1);

    std::puts("channel_select_demo OK");
    return 0;
}
