// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane
#pragma once

// Annex §14.4 rule 1: an `asm` instruction that writes a register, the
// condition flags or a sysreg must name it (an output or inout pin, or a
// clobber). This is the aarch64 model of what each instruction writes.

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

}  // namespace vestra::sema
