// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#pragma once

// Internal helpers shared across the cpp_emitter translation units
// (cpp_emitter.cpp and its cpp_emitter_*.cpp siblings). These were the
// anonymous-namespace free functions that every emission theme leaned on;
// hoisting them here as `inline` lets the file be split by theme without
// duplicating them. Not part of the public codegen API — do not include
// outside src/codegen/.

#include "vestra/ast/nodes.hpp"

#include <ostream>
#include <string>
#include <unordered_map>

namespace vestra::codegen::detail {

// Emit `indent` levels of four-space indentation.
inline void write_indent(std::ostream& os, int indent) {
    for (int i = 0; i < indent; ++i) {
        os << "    ";
    }
}

// Any payload at all on any case means the enum lowers to a
// `std::variant<case_t…>` wrapper, and every case construction needs
// the `Enum{Enum::case_t{…}}` brace-shape. A bare enum lowers to plain
// `enum class Enum { … }` and constructs as `Enum::case` directly.
inline bool enum_is_sum_type(const ast::EnumDecl& e) noexcept {
    for (const auto& c : e.cases) {
        if (!c.payload.empty()) {
            return true;
        }
    }
    return false;
}

// Map Vestra primitive names to C++ equivalents.
inline const std::unordered_map<std::string, std::string>& primitive_map() {
    static const std::unordered_map<std::string, std::string> m = {
        {"Int8", "std::int8_t"},
        {"Int16", "std::int16_t"},
        {"Int32", "std::int32_t"},
        {"Int64", "std::int64_t"},
        {"UInt8", "std::uint8_t"},
        {"UInt16", "std::uint16_t"},
        {"UInt32", "std::uint32_t"},
        {"UInt64", "std::uint64_t"},
        {"Int", "std::intptr_t"},
        {"UInt", "std::uintptr_t"},
        // §A4 (§14.9.4) wide integers. GCC / Clang provide
        // `__int128_t` / `__uint128_t` as compiler intrinsics on
        // every target we care about (aarch64, x86_64, RISC-V at
        // 64-bit). The standard library doesn't have a portable
        // spelling yet — these are intentionally unportable, but
        // they're the platform-blessed way to express the 128-bit
        // integer the §A4 atomic surface needs. The atomic
        // architectures (aarch64 +lse2, x86_64 +cx16) handle
        // `std::atomic<__uint128_t>` natively; on hosts without
        // those features libatomic falls back to a lock-based
        // implementation, which is fine for v0.5 hosted testing.
        {"Int128", "__int128_t"},
        {"UInt128", "__uint128_t"},
        {"Float32", "float"},
        {"Float64", "double"},
        {"Bool", "bool"},
        {"Char", "char32_t"},
        {"Unit", "void"},
        {"String", "std::string"},
        {"Str", "std::string_view"},
        {"StrConst", "std::string_view"},
        // §A10 — `-> Never` in source maps to __vstr::Never on the
        // C++ side. The conversion-op shim handles slotting it into
        // any slot the surrounding expression expects.
        {"Never", "__vstr::Never"},
    };
    return m;
}

}  // namespace vestra::codegen::detail
