// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#pragma once

#include "vestra/sema/comptime.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

// Built-in `comptime` stdlib surface (§12.1). A tiny set of math
// functions and constants that the folder can dispatch to without
// needing a user-written body. Phase 1 keeps the surface intentionally
// small (trig + sqrt + the usual constants) — enough to fold a
// `comptime func` that builds a `[N]Float32` lookup table.
//
// Naming: the spec writes `sin(x)` and `tau` bare (§12.1 example), so
// there's no module path. The resolver registers each of these names
// as a top-level symbol of the right type; the folder dispatches by
// name at fold time.
//
// Width story: every function takes and returns Float64; every constant
// is Float64. A spec example that wants the result in Float32 inserts
// a Float32(...) cast on the way out. A later phase will revisit
// float-kind polymorphism once the resolver grows overload resolution.
namespace vestra::sema::builtins {

// NOLINTNEXTLINE(performance-enum-size)
enum class FuncId : std::uint8_t { Sin, Cos, Tan, Sqrt };

// NOLINTNEXTLINE(performance-enum-size)
enum class ConstId : std::uint8_t { Tau, Pi, E };

struct FuncEntry {
    std::string_view name;
    FuncId id;
    std::size_t arity;  // always 1 in the current set; kept explicit so
                        // later additions (pow, atan2) don't surprise.
};

struct ConstEntry {
    std::string_view name;
    ConstId id;
    double value;
};

// The whole registries. Spans so callers can iterate without taking
// ownership; the underlying arrays live in builtins.cpp.
[[nodiscard]] std::span<const FuncEntry> functions() noexcept;
[[nodiscard]] std::span<const ConstEntry> constants() noexcept;

// Name lookup. Returns nullopt for "not a builtin", which is the
// folder's signal to fall through to its normal call / ident path.
[[nodiscard]] std::optional<FuncEntry> lookup_func(std::string_view name) noexcept;
[[nodiscard]] std::optional<ConstEntry> lookup_const(std::string_view name) noexcept;

// Dispatch a folded call. `args` must have entry.arity entries and
// each must be a Float-kind ComptimeValue; the result is a Float64
// ComptimeValue. Returns nullopt on shape mismatch or a domain error
// (e.g. sqrt of a negative — folder bails to runtime).
[[nodiscard]] std::optional<ComptimeValue> call(FuncEntry entry,
                                                std::span<const ComptimeValue> args) noexcept;

// Construct a Float64 ComptimeValue for the named constant.
[[nodiscard]] ComptimeValue value_of(ConstEntry entry) noexcept;

}  // namespace vestra::sema::builtins
