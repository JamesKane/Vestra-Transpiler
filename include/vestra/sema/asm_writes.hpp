// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane
#pragma once

// Annex §14.4 rule 1: an `asm` instruction that writes a register, the
// condition flags or a sysreg must name it (an output or inout pin, or a
// clobber). These are the per-target models of what each instruction writes:
// aarch64, x86_64 (AT&T syntax, as GCC and Clang read inline asm) and
// riscv64.

#include <string>
#include <string_view>
#include <vector>

namespace vestra::sema {

// One write an instruction makes.
struct AsmWrite {
    enum class Kind { Register, Flags, Sysreg } kind = Kind::Register;
    std::string name;  // canonical register ("x9", "v3") or sysreg ("daif"); empty for Flags
};

// The writes of one template line on aarch64. `{name}` placeholders (operands
// the compiler allocates), the zero registers and `sp` are not reported.
[[nodiscard]] std::vector<AsmWrite> a64_line_writes(std::string_view line);

// "x9" for `x9` / `w9` / `fp`-style spellings, "v3" for `v3` / `q3` / `d3` /
// `s3` / `h3` / `b3`; empty when `tok` is not a general or vector register.
[[nodiscard]] std::string a64_reg_key(std::string_view tok);

// A sysreg name an aarch64 clobber list may carry: `daif`, `nzcv`, any
// `*_el0`..`*_el3` register, or an `s<op0>_<op1>_c<n>_c<m>_<op2>` encoding.
// `daifset` / `daifclr` name `daif`.
[[nodiscard]] bool is_a64_sysreg_name(std::string_view name);
[[nodiscard]] std::string a64_sysreg_key(std::string_view name);

// The writes of one x86_64 template line, in AT&T syntax (destination last,
// registers spelled `%rax`). `{name}` placeholders and `%rsp` are not
// reported; a control-register or MSR write is not modelled.
[[nodiscard]] std::vector<AsmWrite> x86_line_writes(std::string_view line);

// "rax" for `%rax` / `%eax` / `%ax` / `%al` / `%ah`, "r9" for `%r9d` / `%r9w` /
// `%r9b`, "xmm3" for `%xmm3` / `%ymm3` / `%zmm3`, "k1" for `%k1`; empty for
// anything else (and for the stack pointer).
[[nodiscard]] std::string x86_reg_key(std::string_view tok);

// The writes of one riscv64 template line (destination first). The zero
// register, `sp` and `{name}` placeholders are not reported; a CSR write is
// not modelled. RISC-V has no condition flags.
[[nodiscard]] std::vector<AsmWrite> rv_line_writes(std::string_view line);

// The ABI name of a riscv64 register: "a0" for `a0` / `x10`, "s0" for `fp`,
// "fa1" for `f11`; empty for `zero` / `x0`, `sp` and anything else.
[[nodiscard]] std::string rv_reg_key(std::string_view tok);

// The model for `arch` ("aarch64", "x86_64", "riscv64"); no writes and an
// empty key for any other architecture.
[[nodiscard]] std::vector<AsmWrite> asm_line_writes(std::string_view arch, std::string_view line);
[[nodiscard]] std::string asm_reg_key(std::string_view arch, std::string_view tok);

}  // namespace vestra::sema
