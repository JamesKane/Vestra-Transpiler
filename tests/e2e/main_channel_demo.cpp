// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

// E2E driver for the §11 Channel[T] slice: send / recv round-trip, the
// empty-channel nil result, and FIFO order. Also drives the emitted
// __vstr::Channel<T> directly from C++ to confirm the runtime shape.

#include "channel_demo.hpp"

#include <cassert>
#include <cstdio>

namespace cd = examples::channel_demo;

int main() {
    assert(cd::roundtrip() == 60);  // 10 + 20 + 30
    assert(cd::emptyRecv() == -1);  // recv on empty -> nil -> -1
    assert(cd::firstOut() == 1);    // FIFO: first in, first out

    // Drive the emitted Channel directly.
    __vstr::Channel<std::int32_t> ch;
    ch.send(42);
    auto v = ch.recv();
    assert(v.has_value() && *v == 42);
    assert(!ch.recv().has_value());  // now empty

    std::puts("channel_demo OK");
    return 0;
}
