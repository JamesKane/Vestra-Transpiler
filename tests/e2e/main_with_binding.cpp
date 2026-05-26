// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#include "with_binding.hpp"

#include <cstdlib>
#include <print>

int main() {
    using namespace examples::with_binding;

    // totalSize: 256 + 128 = 384.
    if (totalSize() != 384) {
        std::println("totalSize wrong: {}", totalSize());
        return EXIT_FAILURE;
    }

    // nested(7, 9): outer.size(10) + inner.size(20) + outer.base(7) + inner.base(9) = 46.
    if (nested(7, 9) != 46) {
        std::println("nested(7, 9) wrong: {}", nested(7, 9));
        return EXIT_FAILURE;
    }

    std::println("with binding OK");
    return EXIT_SUCCESS;
}
