// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#include "boxes.hpp"

#include <cstdlib>
#include <memory>
#include <print>
#include <type_traits>
#include <utility>

int main() {
    using namespace examples::boxes;

    // mk_box / read_box round-trip.
    auto b = mk_box(42);
    if (read_box(b) != 42) {
        std::println("read_box wrong");
        return EXIT_FAILURE;
    }

    // Box[T] must be std::unique_ptr<T> at the C++ layer — move-only,
    // non-default-copy. Pin both invariants.
    static_assert(std::is_same_v<decltype(b), std::unique_ptr<std::int32_t>>,
                  "Box[Int32] must lower to std::unique_ptr<std::int32_t>");
    static_assert(!std::is_copy_constructible_v<decltype(b)>, "Box[T] must be move-only");
    static_assert(std::is_move_constructible_v<decltype(b)>, "Box[T] must be move-constructible");

    // Cell holding a Box: nominal nesting works.
    auto c = mk_cell(7);
    if (read_cell(std::move(c)) != 7) {
        std::println("read_cell wrong");
        return EXIT_FAILURE;
    }

    std::println("boxes OK");
    return EXIT_SUCCESS;
}
