// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#pragma once

#include <filesystem>
#include <iosfwd>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vestra::driver {

// Top-level options for the `build` command.
struct BuildOptions {
    std::filesystem::path input;
    std::filesystem::path out_dir = ".";
    bool emit_only = false;  // skip writing files; print to stdout
    bool dump_ast = false;   // print the AST printer's output
    bool dump_tokens = false;
    bool skip_check = false;  // skip sema (debug aid; emitter still runs)
    // §A10 (§15.5) freestanding profile. `vestra build --no-libc` is
    // v0.5's equivalent of the manifest flag `profile.freestanding.
    // no_libc = true`; the emitter writes a marker comment in the
    // generated header so downstream link / audit tooling sees the
    // contract.
    bool no_libc = false;
};

// Entry points for each `vestra` subcommand. Each returns a process exit code.
[[nodiscard]] int run(std::span<const std::string_view> argv, std::ostream& out, std::ostream& err);

[[nodiscard]] int run_build(const BuildOptions& opts, std::ostream& out, std::ostream& err);
[[nodiscard]] int
run_check(const std::filesystem::path& input, std::ostream& out, std::ostream& err);
[[nodiscard]] int run_fmt(const std::filesystem::path& input, std::ostream& out, std::ostream& err);

}  // namespace vestra::driver
