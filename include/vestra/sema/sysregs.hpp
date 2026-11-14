// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#pragma once

// §14.12 the target table behind the `Sysreg` namespace: every system
// register Vestra names, with its architecture, how it may be accessed, and
// whether a write needs a context-synchronizing barrier after it (§14.12.3,
// the auto-emitted post-write barrier that `@no_auto_barrier` opts out of).
//
// This is the one list. Sema resolves `Sysreg.<name>` against it, the
// runtime preludes declare one handle per entry, codegen routes the gated
// writes through `write_<name>`, and `vestra audit --sysreg` reports the
// same barrier status. Names are the assembler's lowercase spellings.

#include <array>
#include <string_view>

namespace vestra::sema {

enum class SysregAccess { ReadOnly, ReadWrite, WriteOnly };

struct SysregInfo {
    std::string_view name;
    std::string_view arch;  // "aarch64", "x86_64" or "riscv64"
    SysregAccess access;
    // §14.12.3: a write changes translation, exception or interrupt-routing
    // context, so it needs an ISB before later instructions rely on it.
    bool sync_after_write;
    std::string_view msr;  // x86_64 MSR number; empty elsewhere
};

namespace detail {
using enum SysregAccess;
inline constexpr std::array kSysregTable = std::to_array<SysregInfo>({
    // ---- aarch64: identification and counters (read-only at EL1)
    {"midr_el1", "aarch64", ReadOnly, false, ""},
    {"mpidr_el1", "aarch64", ReadOnly, false, ""},
    {"revidr_el1", "aarch64", ReadOnly, false, ""},
    {"ctr_el0", "aarch64", ReadOnly, false, ""},
    {"dczid_el0", "aarch64", ReadOnly, false, ""},
    {"currentel", "aarch64", ReadOnly, false, ""},
    {"isr_el1", "aarch64", ReadOnly, false, ""},
    {"id_aa64pfr0_el1", "aarch64", ReadOnly, false, ""},
    {"id_aa64pfr1_el1", "aarch64", ReadOnly, false, ""},
    {"id_aa64isar0_el1", "aarch64", ReadOnly, false, ""},
    {"id_aa64isar1_el1", "aarch64", ReadOnly, false, ""},
    {"id_aa64mmfr0_el1", "aarch64", ReadOnly, false, ""},
    {"id_aa64mmfr1_el1", "aarch64", ReadOnly, false, ""},
    {"id_aa64mmfr2_el1", "aarch64", ReadOnly, false, ""},
    {"id_aa64dfr0_el1", "aarch64", ReadOnly, false, ""},
    {"cntfrq_el0", "aarch64", ReadOnly, false, ""},
    {"cntpct_el0", "aarch64", ReadOnly, false, ""},
    {"cntvct_el0", "aarch64", ReadOnly, false, ""},
    // ---- aarch64: EL1 control, translation and exception state
    {"daif", "aarch64", ReadWrite, false, ""},
    {"nzcv", "aarch64", ReadWrite, false, ""},
    {"spsel", "aarch64", ReadWrite, false, ""},
    {"sp_el0", "aarch64", ReadWrite, false, ""},
    {"sctlr_el1", "aarch64", ReadWrite, true, ""},
    {"actlr_el1", "aarch64", ReadWrite, true, ""},
    {"cpacr_el1", "aarch64", ReadWrite, true, ""},
    {"vbar_el1", "aarch64", ReadWrite, true, ""},
    {"ttbr0_el1", "aarch64", ReadWrite, true, ""},
    {"ttbr1_el1", "aarch64", ReadWrite, true, ""},
    {"tcr_el1", "aarch64", ReadWrite, true, ""},
    {"mair_el1", "aarch64", ReadWrite, true, ""},
    {"amair_el1", "aarch64", ReadWrite, true, ""},
    {"contextidr_el1", "aarch64", ReadWrite, true, ""},
    {"mdscr_el1", "aarch64", ReadWrite, true, ""},
    {"par_el1", "aarch64", ReadWrite, false, ""},
    {"esr_el1", "aarch64", ReadWrite, false, ""},
    {"far_el1", "aarch64", ReadWrite, false, ""},
    {"elr_el1", "aarch64", ReadWrite, false, ""},
    {"spsr_el1", "aarch64", ReadWrite, false, ""},
    {"tpidr_el1", "aarch64", ReadWrite, false, ""},
    {"tpidr_el0", "aarch64", ReadWrite, false, ""},
    {"tpidrro_el0", "aarch64", ReadWrite, false, ""},
    {"fpcr", "aarch64", ReadWrite, false, ""},
    {"fpsr", "aarch64", ReadWrite, false, ""},
    // ---- aarch64: generic timer (EL1 physical / virtual, EL0 access control)
    {"cntkctl_el1", "aarch64", ReadWrite, false, ""},
    {"cntp_ctl_el0", "aarch64", ReadWrite, false, ""},
    {"cntp_cval_el0", "aarch64", ReadWrite, false, ""},
    {"cntp_tval_el0", "aarch64", ReadWrite, false, ""},
    {"cntv_ctl_el0", "aarch64", ReadWrite, false, ""},
    {"cntv_cval_el0", "aarch64", ReadWrite, false, ""},
    {"cntv_tval_el0", "aarch64", ReadWrite, false, ""},
    // ---- aarch64: GICv3 CPU interface. Acknowledge / pending / running
    // priority are read-only; end-of-interrupt, deactivate and SGI
    // generation are write-only.
    {"icc_sre_el1", "aarch64", ReadWrite, true, ""},
    {"icc_ctlr_el1", "aarch64", ReadWrite, true, ""},
    {"icc_igrpen0_el1", "aarch64", ReadWrite, true, ""},
    {"icc_igrpen1_el1", "aarch64", ReadWrite, true, ""},
    {"icc_pmr_el1", "aarch64", ReadWrite, false, ""},
    {"icc_bpr0_el1", "aarch64", ReadWrite, false, ""},
    {"icc_bpr1_el1", "aarch64", ReadWrite, false, ""},
    {"icc_ap0r0_el1", "aarch64", ReadWrite, false, ""},
    {"icc_ap1r0_el1", "aarch64", ReadWrite, false, ""},
    {"icc_iar0_el1", "aarch64", ReadOnly, false, ""},
    {"icc_iar1_el1", "aarch64", ReadOnly, false, ""},
    {"icc_hppir0_el1", "aarch64", ReadOnly, false, ""},
    {"icc_hppir1_el1", "aarch64", ReadOnly, false, ""},
    {"icc_rpr_el1", "aarch64", ReadOnly, false, ""},
    {"icc_eoir0_el1", "aarch64", WriteOnly, false, ""},
    {"icc_eoir1_el1", "aarch64", WriteOnly, false, ""},
    {"icc_dir_el1", "aarch64", WriteOnly, false, ""},
    {"icc_sgi0r_el1", "aarch64", WriteOnly, false, ""},
    {"icc_sgi1r_el1", "aarch64", WriteOnly, false, ""},
    {"icc_asgi1r_el1", "aarch64", WriteOnly, false, ""},
    // ---- x86_64 MSRs (ring 0), by number
    {"ia32_tsc", "x86_64", ReadWrite, false, "0x10u"},
    {"ia32_apic_base", "x86_64", ReadWrite, false, "0x1Bu"},
    {"ia32_pat", "x86_64", ReadWrite, false, "0x277u"},
    {"ia32_efer", "x86_64", ReadWrite, false, "0xC0000080u"},
    {"ia32_star", "x86_64", ReadWrite, false, "0xC0000081u"},
    {"ia32_lstar", "x86_64", ReadWrite, false, "0xC0000082u"},
    {"ia32_fmask", "x86_64", ReadWrite, false, "0xC0000084u"},
    {"ia32_fs_base", "x86_64", ReadWrite, false, "0xC0000100u"},
    {"ia32_gs_base", "x86_64", ReadWrite, false, "0xC0000101u"},
    {"ia32_kernel_gs_base", "x86_64", ReadWrite, false, "0xC0000102u"},
    {"ia32_tsc_aux", "x86_64", ReadWrite, false, "0xC0000103u"},
    // ---- RISC-V S-mode CSRs, and the unprivileged counters (read-only)
    {"sstatus", "riscv64", ReadWrite, false, ""},
    {"sie", "riscv64", ReadWrite, false, ""},
    {"sip", "riscv64", ReadWrite, false, ""},
    {"stvec", "riscv64", ReadWrite, false, ""},
    {"scounteren", "riscv64", ReadWrite, false, ""},
    {"senvcfg", "riscv64", ReadWrite, false, ""},
    {"sscratch", "riscv64", ReadWrite, false, ""},
    {"sepc", "riscv64", ReadWrite, false, ""},
    {"scause", "riscv64", ReadWrite, false, ""},
    {"stval", "riscv64", ReadWrite, false, ""},
    {"satp", "riscv64", ReadWrite, false, ""},
    {"time", "riscv64", ReadOnly, false, ""},
    {"cycle", "riscv64", ReadOnly, false, ""},
    {"instret", "riscv64", ReadOnly, false, ""},
});
}  // namespace detail

inline constexpr const auto& sysreg_table() noexcept {
    return detail::kSysregTable;
}

inline constexpr const SysregInfo* find_sysreg(std::string_view name) noexcept {
    for (const auto& r : detail::kSysregTable) {
        if (r.name == name) {
            return &r;
        }
    }
    return nullptr;
}

// True when `Sysreg.<name>.write(v)` gets the auto-emitted post-write barrier.
inline constexpr bool sysreg_write_is_gated(std::string_view name) noexcept {
    const auto* r = find_sysreg(name);
    return r != nullptr && r->sync_after_write;
}

}  // namespace vestra::sema
