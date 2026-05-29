// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

// E2E driver for the §11 async first slice. Drives each async function to
// completion from a non-async (C++) caller via Task::get(), confirming the
// coroutine lowering of `async func` + `await` runs with correct
// sequential semantics.

#include "async_demo.hpp"

#include <cassert>
#include <cstdio>

namespace ad = examples::async_demo;

int main() {
    // Single leaf await.
    assert(ad::increment(41).get() == 42);

    // Two sequential awaits, summed.
    assert(ad::sumTwo(10, 20).get() == 32);  // (10+1) + (20+1)

    // Nested await: increment(increment(5)) == 7.
    assert(ad::twice(5).get() == 7);

    // Void async function runs to completion.
    ad::touch(0).get();

    // spawn → Future[T], consumed by await.
    assert(ad::sumSpawned(10, 20).get() == 32);  // (10+1) + (20+1)

    std::puts("async_demo OK");
    return 0;
}
