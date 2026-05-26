// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#include "link_attrs.hpp"

#include <cstdlib>
#include <print>

int main() {
    using namespace examples::link_attrs;

    // The @noinit static lands in .bss; on a hosted target C++
    // zero-initializes the storage, so the first byte reads as 0.
    if (auto v = first_cap_byte(); v != 0) {
        std::println("first_cap_byte expected 0, got {}", static_cast<int>(v));
        return EXIT_FAILURE;
    }
    // cap_count carries an initializer + @section(".rodata...").
    if (cap_count != 16) {
        std::println("cap_count expected 16, got {}", cap_count);
        return EXIT_FAILURE;
    }
    // The weak default returns -1; if a strong override were linked
    // in, it would shadow this body. We just confirm the body runs.
    if (default_trap_handler() != -1) {
        std::println("default_trap_handler should return -1");
        return EXIT_FAILURE;
    }
    // kernel_entry runs through the asm-label trampoline: the
    // Vestra-side identifier is `kernel_entry`, but the emitted
    // symbol is `kernel_entry_symbol`. Source-level callers don't
    // see the rename.
    if (kernel_entry() != 42) {
        std::println("kernel_entry expected 42");
        return EXIT_FAILURE;
    }

    std::println("link_attrs OK");
    return EXIT_SUCCESS;
}
