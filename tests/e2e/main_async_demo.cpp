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

    // spawn of a void async fn → Future[Unit] (lowered to Future<void>);
    // awaiting it completes the task, then the caller continues. 41 -> 42.
    assert(ad::awaitVoidSpawn(41).get() == 42);

    // spawn → Future[T], consumed by await.
    assert(ad::sumSpawned(10, 20).get() == 32);  // (10+1) + (20+1)

    // select over two ready futures: the first arm wins.
    assert(ad::firstReady(10, 20).get() == 11);  // increment(10)

    // async + throws -> Task<expected<T, E>>. Success carries the value; a
    // throw / propagated error surfaces as an errored expected.
    assert(ad::checkedInc(5).get().value() == 6);
    assert(!ad::checkedInc(-1).get().has_value());  // throw -> error
    assert(ad::incTwice(5).get().value() == 7);     // 5 -> 6 -> 7
    assert(!ad::incTwice(-1).get().has_value());    // try propagates the error

    std::puts("async_demo OK");
    return 0;
}
