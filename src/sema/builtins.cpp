// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#include "vestra/sema/builtins.hpp"

#include "vestra/sema/comptime.hpp"
#include "vestra/sema/types.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <optional>
#include <span>
#include <string_view>

namespace vestra::sema::builtins {

namespace {

constexpr std::array Functions = {
    FuncEntry{"sin", FuncId::Sin, 1},
    FuncEntry{"cos", FuncId::Cos, 1},
    FuncEntry{"tan", FuncId::Tan, 1},
    FuncEntry{"sqrt", FuncId::Sqrt, 1},
};

constexpr std::array Constants = {
    // C++26's <numbers> gives us the canonical bit-pattern; using these
    // here keeps Vestra's `tau` exactly equal to the corresponding
    // double-precision C++ constant, which avoids a rounding gap if a
    // user's runtime code does its own `2.0 * std::numbers::pi`.
    ConstEntry{"tau", ConstId::Tau, 2.0 * std::numbers::pi_v<double>},
    ConstEntry{"pi", ConstId::Pi, std::numbers::pi_v<double>},
    ConstEntry{"e", ConstId::E, std::numbers::e_v<double>},
};

}  // namespace

std::span<const FuncEntry> functions() noexcept {
    return Functions;
}

std::span<const ConstEntry> constants() noexcept {
    return Constants;
}

std::optional<FuncEntry> lookup_func(std::string_view name) noexcept {
    for (const auto& f : Functions) {
        if (f.name == name) {
            return f;
        }
    }
    return std::nullopt;
}

std::optional<ConstEntry> lookup_const(std::string_view name) noexcept {
    for (const auto& c : Constants) {
        if (c.name == name) {
            return c;
        }
    }
    return std::nullopt;
}

ComptimeValue value_of(ConstEntry entry) noexcept {
    ComptimeValue cv;
    cv.kind = ComptimeValue::Kind::Float;
    cv.type = TypeKind::Float64;
    cv.f = entry.value;
    return cv;
}

std::optional<ComptimeValue> call(FuncEntry entry, std::span<const ComptimeValue> args) noexcept {
    if (args.size() != entry.arity) {
        return std::nullopt;
    }
    // All current builtins want Float-kind arguments. A non-Float folded
    // value here means the resolver passed something through that we
    // don't yet handle (e.g. an Int arg that needed Float coercion).
    for (const auto& a : args) {
        if (a.kind != ComptimeValue::Kind::Float) {
            return std::nullopt;
        }
    }

    double result = 0.0;
    switch (entry.id) {
    case FuncId::Sin:
        result = std::sin(args[0].f);
        break;
    case FuncId::Cos:
        result = std::cos(args[0].f);
        break;
    case FuncId::Tan:
        result = std::tan(args[0].f);
        break;
    case FuncId::Sqrt:
        if (args[0].f < 0.0) {
            // sqrt of a negative would produce NaN — bail so the caller
            // can choose to fall back to runtime (where the same call
            // will at least raise an FE_INVALID consistently).
            return std::nullopt;
        }
        result = std::sqrt(args[0].f);
        break;
    }

    ComptimeValue cv;
    cv.kind = ComptimeValue::Kind::Float;
    cv.type = TypeKind::Float64;
    cv.f = result;
    return cv;
}

}  // namespace vestra::sema::builtins
