// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#include "mmio_wire.hpp"

#include <bit>
#include <cstdint>
#include <cstdlib>
#include <print>

int main() {
    using namespace examples::mmio_wire;

    constexpr std::uint32_t host_value = 0x11223344;
    constexpr std::uint32_t swapped = 0x44332211;
    const bool host_little = (std::endian::native == std::endian::little);

    // Big-endian wire round-trip: the user passes host-order, the
    // wrapper writes bus-order (swapped on LE hosts), and the read
    // swaps back to host-order. The raw cell should hold the
    // bus-order bytes after the write.
    std::uint32_t cell = 0;
    auto cell_addr = reinterpret_cast<std::uint64_t>(&cell);
    if (auto v = talk_big(cell_addr, host_value); v != host_value) {
        std::println("talk_big round-trip expected {:#x}, got {:#x}", host_value, v);
        return EXIT_FAILURE;
    }
    const std::uint32_t expected_raw = host_little ? swapped : host_value;
    if (cell != expected_raw) {
        std::println("talk_big raw cell expected {:#x}, got {:#x}", expected_raw, cell);
        return EXIT_FAILURE;
    }

    // Native: writes the host-order value verbatim and reads it
    // back unchanged on both host endians.
    cell = 0;
    if (auto v = talk_native(cell_addr, host_value); v != host_value) {
        std::println("talk_native round-trip expected {:#x}, got {:#x}", host_value, v);
        return EXIT_FAILURE;
    }
    if (cell != host_value) {
        std::println("talk_native raw cell expected {:#x}, got {:#x}", host_value, cell);
        return EXIT_FAILURE;
    }

    std::println("mmio_wire OK");
    return EXIT_SUCCESS;
}
