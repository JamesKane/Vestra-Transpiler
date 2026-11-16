// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane
#pragma once

// §17.1 escapes in a string or char literal's raw text. Vestra's escapes are
// C++'s, except the Unicode scalar escape `\u{HEX}` (one to six hex digits),
// which C++ spells `\UXXXXXXXX`.

#include <cstdint>
#include <format>
#include <string>
#include <string_view>

namespace vestra::ast {

[[nodiscard]] inline bool is_hex_digit(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

// The raw text with every `\u{HEX}` rewritten as C++'s `\UXXXXXXXX`.
[[nodiscard]] inline std::string cpp_escapes(std::string_view raw) {
    std::string out;
    out.reserve(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '\\' && i + 1 < raw.size()) {
            if (raw[i + 1] == 'u' && i + 2 < raw.size() && raw[i + 2] == '{') {
                const auto close = raw.find('}', i + 3);
                if (close != std::string_view::npos) {
                    std::uint32_t v = 0;
                    for (std::size_t k = i + 3; k < close; ++k) {
                        const char c = raw[k];
                        v = v * 16
                            + static_cast<std::uint32_t>(c <= '9'   ? c - '0'
                                                         : c <= 'F' ? c - 'A' + 10
                                                                    : c - 'a' + 10);
                    }
                    out += std::format("\\U{:08X}", v);
                    i = close;
                    continue;
                }
            }
            out += raw[i];
            out += raw[i + 1];
            ++i;
            continue;
        }
        out += raw[i];
    }
    return out;
}

}  // namespace vestra::ast
