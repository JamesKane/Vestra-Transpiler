// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#include "addr_of.hpp"

#include <cstdlib>
#include <print>

int main() {
    using namespace examples::addr_of;

    // config_ptr() returns &config_word as a Ptr[UInt32] →
    // `const std::uint32_t*` in C++. Read through it and confirm
    // we get the static's initial value.
    const std::uint32_t* p = config_ptr();
    if (p == nullptr) {
        std::println("config_ptr returned null");
        return EXIT_FAILURE;
    }
    if (*p != 0xCAFE'BABE) {
        std::println("config_word should be 0xCAFEBABE, got {:#x}", *p);
        return EXIT_FAILURE;
    }
    // The pointer must equal &config_word — that's the load-bearing
    // address-of-static property.
    if (p != &config_word) {
        std::println("config_ptr should equal &config_word");
        return EXIT_FAILURE;
    }

    // buffer_ptr() yields &rx_buffer. Confirm the address matches
    // the static's storage. Reading through the pointer is gated
    // on RawMemory at the spec layer; v0.5 doesn't enforce that on
    // the read side, but we just check the address here.
    const std::array<std::uint8_t, 16>* bp = buffer_ptr();
    if (bp != &rx_buffer) {
        std::println("buffer_ptr should equal &rx_buffer");
        return EXIT_FAILURE;
    }

    // @extern("C") @symbol("vestra_helper") — the function compiles
    // with the rename. Call it and verify the body runs.
    if (helper() != 42) {
        std::println("helper should return 42");
        return EXIT_FAILURE;
    }

    std::println("addr_of OK");
    return EXIT_SUCCESS;
}
