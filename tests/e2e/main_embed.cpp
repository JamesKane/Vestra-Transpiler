// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#include "embed.hpp"

#include <print>

int main() {
    // "Hello, embed!\n" — the file embedded at compile time. The
    // checksum is the unsigned sum of every byte (1128 for these
    // 14 bytes); the first byte should be 'H' (72), the last '\n'
    // (10). All three numbers come straight out of the embedded
    // `inline constexpr std::array<std::uint8_t, 14>` literal in
    // the emitted header.
    std::println("checksum() = {}", examples::embeds::checksum());
    std::println(
        "first = {}, last = {}", examples::embeds::byte_at(0), examples::embeds::byte_at(13));
    return 0;
}
