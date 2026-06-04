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
    // §A4 (§14.9.4) target-feature gating for wide atomics + lock-
    // free pointer swaps. v0.5's hosted default keeps `target=host`
    // so the host C++ compiler decides — `std::atomic<__uint128_t>`
    // falls back to libatomic locks on hosts without +lse2/+cx16,
    // which is correctness-equivalent for tests. When the user
    // specifies a real target (`--target=aarch64` etc.) sema
    // requires the matching feature in the features set; a missing
    // feature errors at compile time rather than silently degrading
    // to a locked implementation. The same gate fires for
    // AtomicTaggedPointer[T] since it builds on the wide-atomic
    // primitive.
    std::string target = "host";
    std::vector<std::string> target_features;
    // §5 import search roots (repeatable `-I DIR`). An `import a.b.c` resolves
    // against the entry file's directory first, then each of these in order;
    // the first existing `a/b/c.vst` wins. Lets a build name shared library
    // modules from a common root regardless of where the entry file lives.
    std::vector<std::filesystem::path> import_paths;
    // §12.1 / §8 content-hashed `@embed` manifest. `embed_manifest` (enforce):
    // every `@embed`ed file's content hash + size must match an entry in this
    // manifest, or the build fails — pinning exactly which bytes a build
    // embeds so two builds of the same source are byte-identical.
    // `emit_embed_manifest` (generate): after a clean build, write the manifest
    // of every embedded file. At most one may be set. Empty = no manifest.
    std::filesystem::path embed_manifest;
    std::filesystem::path emit_embed_manifest;
};

// Entry points for each `vestra` subcommand. Each returns a process exit code.
[[nodiscard]] int run(std::span<const std::string_view> argv, std::ostream& out, std::ostream& err);

[[nodiscard]] int run_build(const BuildOptions& opts, std::ostream& out, std::ostream& err);
// §5 `import_paths` are extra import search roots (see BuildOptions); empty for
// a plain single-root check.
[[nodiscard]] int run_check(const std::filesystem::path& input,
                            std::ostream& out,
                            std::ostream& err,
                            const std::vector<std::filesystem::path>& import_paths = {});
[[nodiscard]] int run_fmt(const std::filesystem::path& input, std::ostream& out, std::ostream& err);
[[nodiscard]] int
run_expand(const std::filesystem::path& input, std::ostream& out, std::ostream& err);

// §14.12.3 / §15.5 audit surface. `vestra audit <file.vst>` enumerates
// every site that crossed a discipline-bearing boundary so a cross-
// architecture review can verify the build's contracts at the call-
// site level. v0.5 first slice ships --sysreg; --no-libc and the
// cross-architecture barrier-table verification queue for follow-up.
struct AuditOptions {
    std::filesystem::path input;
    bool sysreg = false;
    bool no_libc = false;
};
[[nodiscard]] int run_audit(const AuditOptions& opts, std::ostream& out, std::ostream& err);

}  // namespace vestra::driver
