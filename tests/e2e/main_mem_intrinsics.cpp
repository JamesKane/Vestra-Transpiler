// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#include "mem_intrinsics.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <print>

int main() {
    using namespace examples::mem_intrinsics;

    // memcpy: copy 8 bytes from src to dst.
    std::array<std::uint8_t, 8> src{0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80};
    std::array<std::uint8_t, 8> dst{};
    copy_bytes(reinterpret_cast<std::uint64_t>(dst.data()),
               reinterpret_cast<std::uint64_t>(src.data()),
               8);
    for (std::size_t i = 0; i < 8; ++i) {
        if (dst[i] != src[i]) {
            std::println("memcpy mismatch at index {}: dst={:#x} src={:#x}", i, dst[i], src[i]);
            return EXIT_FAILURE;
        }
    }

    // memset: fill 4 bytes with 0xAB.
    std::array<std::uint8_t, 6> buf{1, 2, 3, 4, 5, 6};
    fill_bytes(reinterpret_cast<std::uint64_t>(buf.data()), 0xAB, 4);
    constexpr std::uint8_t expected_set[] = {0xAB, 0xAB, 0xAB, 0xAB, 5, 6};
    for (std::size_t i = 0; i < 6; ++i) {
        if (buf[i] != expected_set[i]) {
            std::println("memset mismatch at index {}: got {:#x} expected {:#x}",
                         i,
                         buf[i],
                         expected_set[i]);
            return EXIT_FAILURE;
        }
    }

    // memmove with overlap: shift the contents of a buffer up by
    // one. Source range [0..7] copied to destination [1..8]
    // overlaps the source — memmove handles the overlap; a plain
    // memcpy would corrupt the result.
    std::array<std::uint8_t, 8> ov{1, 2, 3, 4, 5, 6, 7, 8};
    shift_bytes(reinterpret_cast<std::uint64_t>(ov.data() + 1),
                reinterpret_cast<std::uint64_t>(ov.data()),
                7);
    constexpr std::uint8_t expected_mv[] = {1, 1, 2, 3, 4, 5, 6, 7};
    for (std::size_t i = 0; i < 8; ++i) {
        if (ov[i] != expected_mv[i]) {
            std::println(
                "memmove mismatch at index {}: got {:#x} expected {:#x}", i, ov[i], expected_mv[i]);
            return EXIT_FAILURE;
        }
    }

    std::println("mem_intrinsics OK");
    return EXIT_SUCCESS;
}
