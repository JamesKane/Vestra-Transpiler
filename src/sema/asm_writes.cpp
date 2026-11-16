// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#include "vestra/sema/asm_writes.hpp"

#include <algorithm>
#include <array>
#include <cctype>

namespace vestra::sema {

namespace {

std::string lower(std::string_view s) {
    std::string out(s);
    for (auto& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())) != 0) {
        s.remove_prefix(1);
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())) != 0) {
        s.remove_suffix(1);
    }
    return s;
}

bool all_digits(std::string_view s) {
    return !s.empty() && std::ranges::all_of(s, [](char c) { return c >= '0' && c <= '9'; });
}

// Split operands at top-level commas (not inside `[...]`, `{...}` or `(...)`).
std::vector<std::string_view> split_operands(std::string_view s) {
    std::vector<std::string_view> out;
    int depth = 0;
    std::size_t start = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '[' || c == '{' || c == '(') {
            ++depth;
        } else if (c == ']' || c == '}' || c == ')') {
            --depth;
        } else if (c == ',' && depth == 0) {
            out.push_back(trim(s.substr(start, i - start)));
            start = i + 1;
        }
    }
    const auto last = trim(s.substr(start));
    if (!last.empty()) {
        out.push_back(last);
    }
    return out;
}

bool starts_with_any(std::string_view m, std::initializer_list<std::string_view> ps) {
    return std::ranges::any_of(ps, [&](std::string_view p) { return m.starts_with(p); });
}

bool one_of(std::string_view m, std::initializer_list<std::string_view> ns) {
    return std::ranges::any_of(ns, [&](std::string_view n) { return m == n; });
}

// The base register of a memory operand `[xN, ...]`.
std::string mem_base(std::string_view op) {
    if (op.empty() || op.front() != '[') {
        return {};
    }
    auto inner = op.substr(1);
    const auto end = inner.find_first_of(",]");
    return a64_reg_key(inner.substr(0, end));
}

void add_reg(std::vector<AsmWrite>& out, std::string_view op) {
    auto key = a64_reg_key(op);
    if (!key.empty()) {
        out.push_back({AsmWrite::Kind::Register, std::move(key)});
    }
}

// Drop a leading label (`1:` or `name:`) and surrounding space.
std::string_view strip_label(std::string_view stmt) {
    stmt = trim(stmt);
    if (const auto colon = stmt.find(':'); colon != std::string_view::npos) {
        const auto head = stmt.substr(0, colon);
        if (!head.empty() && std::ranges::all_of(head, [](char c) {
                return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '.';
            })) {
            stmt = trim(stmt.substr(colon + 1));
        }
    }
    return stmt;
}

// Run `each` on every `;`-separated statement of `line`, after cutting the
// line at its comment marker.
template <typename F>
void each_statement(std::string_view line, std::string_view comment, F each) {
    if (const auto c = line.find(comment); c != std::string_view::npos) {
        line = line.substr(0, c);
    }
    std::size_t start = 0;
    for (std::size_t i = 0; i <= line.size(); ++i) {
        if (i == line.size() || line[i] == ';') {
            each(line.substr(start, i - start));
            start = i + 1;
        }
    }
}

void one_statement(std::string_view stmt, std::vector<AsmWrite>& out) {
    stmt = strip_label(stmt);
    if (stmt.empty() || stmt.front() == '.') {
        return;  // empty, or an assembler directive
    }
    const auto sp = stmt.find_first_of(" \t");
    const std::string m = lower(stmt.substr(0, sp));
    const auto ops =
        sp == std::string_view::npos ? std::vector<std::string_view>{} : split_operands(stmt.substr(sp));
    auto op = [&](std::size_t i) { return i < ops.size() ? ops[i] : std::string_view{}; };

    if (m == "msr") {
        const auto sys = lower(op(0));
        if (sys == "nzcv") {
            out.push_back({AsmWrite::Kind::Flags, {}});
        } else if (!sys.empty()) {
            out.push_back({AsmWrite::Kind::Sysreg, a64_sysreg_key(sys)});
        }
        return;
    }
    if (one_of(m, {"cmp", "cmn", "tst", "ccmp", "ccmn", "fcmp", "fcmpe", "fccmp", "fccmpe"})) {
        out.push_back({AsmWrite::Kind::Flags, {}});
        return;
    }
    if (one_of(m, {"bl", "blr", "blraa", "blrab", "blraaz", "blrabz"})) {
        out.push_back({AsmWrite::Kind::Register, "x30"});
        return;
    }
    if (m.starts_with("b.") || m.starts_with("bc.")
        || one_of(m, {"b", "br", "ret", "cbz", "cbnz", "tbz", "tbnz", "eret", "nop", "isb", "dsb",
                      "dmb", "wfi", "wfe", "sev", "sevl", "yield", "hint", "svc", "hvc", "smc",
                      "brk", "hlt", "clrex", "prfm", "prfum", "dc", "ic", "tlbi", "at", "sys",
                      "bti", "csdb", "sb", "esb", "ssbb", "pssbb", "braa", "brab", "braaz",
                      "brabz", "retaa", "retab", "eretaa", "eretab"})) {
        return;
    }
    // Base-register writeback: `[xN, #i]!` (pre-index) or `[xN], #i`
    // (post-index) on a load or store.
    if (m.starts_with("ld") || m.starts_with("st")) {
        for (std::size_t i = 0; i < ops.size(); ++i) {
            const auto o = ops[i];
            const bool pre = o.size() > 1 && o.back() == '!' && o.front() == '[';
            const bool post = !o.empty() && o.front() == '[' && o.back() == ']'
                              && o.find(',') == std::string_view::npos && i + 1 < ops.size();
            if (pre || post) {
                if (auto b = mem_base(o); !b.empty()) {
                    out.push_back({AsmWrite::Kind::Register, std::move(b)});
                }
            }
        }
    }
    if (starts_with_any(m, {"stxr", "stlxr", "stxp", "stlxp"})) {
        add_reg(out, op(0));  // the status register
        return;
    }
    if (m.starts_with("st")) {
        return;  // a store (or an atomic with no result) writes no register
    }
    if (one_of(m, {"ldp", "ldnp", "ldpsw", "ldxp", "ldaxp"})) {
        add_reg(out, op(0));
        add_reg(out, op(1));
        return;
    }
    if (starts_with_any(m, {"swp", "ldadd", "ldclr", "ldeor", "ldset", "ldsmax", "ldsmin", "ldumax",
                            "ldumin"})) {
        add_reg(out, op(1));
        return;
    }
    if (one_of(m, {"adds", "subs", "ands", "bics", "adcs", "sbcs", "negs", "ngcs"})) {
        out.push_back({AsmWrite::Kind::Flags, {}});
    }
    add_reg(out, op(0));  // the destination
}

}  // namespace

std::string a64_reg_key(std::string_view tok) {
    const std::string t = lower(trim(tok));
    if (t == "fp") {
        return "x29";
    }
    if (t == "lr") {
        return "x30";
    }
    if (t.size() < 2) {
        return {};
    }
    std::string_view body = std::string_view(t).substr(1);
    if (const auto dot = body.find('.'); dot != std::string_view::npos) {
        body = body.substr(0, dot);  // `v3.16b`
    }
    if (!all_digits(body) || (body.size() > 1 && body.front() == '0')) {
        return {};
    }
    const int n = std::stoi(std::string(body));
    const char c = t.front();
    if ((c == 'x' || c == 'w') && n <= 30) {
        return "x" + std::to_string(n);
    }
    if ((c == 'v' || c == 'q' || c == 'd' || c == 's' || c == 'h' || c == 'b') && n <= 31) {
        return "v" + std::to_string(n);
    }
    return {};
}

std::string a64_sysreg_key(std::string_view name) {
    std::string n = lower(name);
    if (n == "daifset" || n == "daifclr") {
        return "daif";
    }
    return n;
}

bool is_a64_sysreg_name(std::string_view name) {
    const std::string n = lower(name);
    if (one_of(n, {"daif", "daifset", "daifclr", "nzcv", "spsel", "fpcr", "fpsr", "pan", "uao",
                   "dit", "ssbs", "tco", "currentel"})) {
        return true;
    }
    // `*_el0` .. `*_el3`
    if (n.size() > 4 && n[n.size() - 4] == '_' && n[n.size() - 3] == 'e' && n[n.size() - 2] == 'l'
        && n.back() >= '0' && n.back() <= '3') {
        return std::ranges::all_of(n, [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
        });
    }
    // `s3_0_c4_c2_1`
    if (n.size() > 1 && n.front() == 's' && n[1] >= '0' && n[1] <= '3') {
        return n.find("_c") != std::string::npos;
    }
    return false;
}

std::vector<AsmWrite> a64_line_writes(std::string_view line) {
    // A `//` comment ends the line; `;` separates statements.
    std::vector<AsmWrite> out;
    each_statement(line, "//", [&](std::string_view st) { one_statement(st, out); });
    return out;
}

// ----- x86_64 ---------------------------------------------------------------

namespace {

// `m` is `base`, or `base` with an AT&T size suffix (`addq`, `shlb`).
bool x86_is(std::string_view m, std::initializer_list<std::string_view> bases) {
    return std::ranges::any_of(bases, [&](std::string_view b) {
        return m == b
               || (m.size() == b.size() + 1 && m.starts_with(b)
                   && std::string_view("bwlq").find(m.back()) != std::string_view::npos);
    });
}

void x86_add(std::vector<AsmWrite>& out, std::string_view op) {
    // `%zmm3{%k1}`: the register before an AVX-512 mask; a `{name}`
    // placeholder starts with the brace and is skipped.
    if (const auto brace = op.find('{'); brace != std::string_view::npos && brace > 0) {
        op = op.substr(0, brace);
    }
    auto key = x86_reg_key(op);
    if (!key.empty()) {
        out.push_back({AsmWrite::Kind::Register, std::move(key)});
    }
}

void x86_regs(std::vector<AsmWrite>& out, std::initializer_list<std::string_view> regs) {
    for (auto r : regs) {
        out.push_back({AsmWrite::Kind::Register, std::string(r)});
    }
}

void x86_statement(std::string_view stmt, std::vector<AsmWrite>& out, bool rep) {
    stmt = strip_label(stmt);
    if (stmt.empty() || stmt.front() == '.') {
        return;
    }
    const auto sp = stmt.find_first_of(" \t");
    const std::string m = lower(stmt.substr(0, sp));
    const auto rest = sp == std::string_view::npos ? std::string_view{} : stmt.substr(sp);
    // A prefix: the instruction after it is what writes.
    if (one_of(m, {"lock", "rep", "repe", "repz", "repne", "repnz", "notrack"})) {
        x86_statement(rest, out, rep || m.starts_with("rep"));
        return;
    }
    const auto ops = rest.empty() ? std::vector<std::string_view>{} : split_operands(rest);
    auto last = [&] { return ops.empty() ? std::string_view{} : ops.back(); };

    // String instructions (operand-less forms): their pointer registers
    // advance, and a `rep` prefix counts %rcx down.
    if (ops.empty()) {
        auto str = [&](std::string_view b) {
            return m == b || (m.size() == b.size() + 1 && m.starts_with(b)
                              && std::string_view("bwdlq").find(m.back()) != std::string_view::npos);
        };
        bool string_op = true;
        if (str("movs")) {
            x86_regs(out, {"rsi", "rdi"});
        } else if (str("stos") || str("ins")) {
            x86_regs(out, {"rdi"});
        } else if (str("lods")) {
            x86_regs(out, {"rax", "rsi"});
        } else if (str("outs")) {
            x86_regs(out, {"rsi"});
        } else if (str("cmps")) {
            x86_regs(out, {"rsi", "rdi"});
            out.push_back({AsmWrite::Kind::Flags, {}});
        } else if (str("scas")) {
            x86_regs(out, {"rdi"});
            out.push_back({AsmWrite::Kind::Flags, {}});
        } else {
            string_op = false;
        }
        if (string_op) {
            if (rep) {
                x86_regs(out, {"rcx"});
            }
            return;
        }
    }

    if (x86_is(m, {"add", "adc", "sub", "sbb", "and", "or", "xor", "cmp", "test", "neg", "inc",
                   "dec", "shl", "shr", "sal", "sar", "rol", "ror", "rcl", "rcr", "shld", "shrd",
                   "imul", "mul", "div", "idiv", "bt", "bts", "btr", "btc", "bsf", "bsr",
                   "popcnt", "lzcnt", "tzcnt", "cmpxchg", "xadd", "andn", "bextr", "blsi",
                   "blsmsk", "blsr", "adcx", "adox", "rdrand", "rdseed"})
        || one_of(m, {"cmpxchg8b", "cmpxchg16b", "sahf", "popf", "popfq", "stc", "clc", "cmc",
                      "std", "cld", "comiss", "comisd", "ucomiss", "ucomisd", "vcomiss", "vcomisd",
                      "vucomiss", "vucomisd", "ptest", "vptest"})) {
        out.push_back({AsmWrite::Kind::Flags, {}});
    }
    // Flags only, or no register at all.
    if (x86_is(m, {"cmp", "test", "bt", "push"}) || m.starts_with('j') || m.starts_with("prefetch")
        || one_of(m, {"sahf", "popf", "popfq", "stc", "clc", "cmc", "std", "cld", "comiss",
                      "comisd", "ucomiss", "ucomisd", "vcomiss", "vcomisd", "vucomiss",
                      "vucomisd", "ptest", "vptest", "call", "callq", "ret", "retq", "nop", "pause",
                      "hlt", "cli", "sti", "lfence", "mfence", "sfence", "invlpg", "invpcid", "int",
                      "int3", "into", "ud2", "lgdt", "lidt", "ltr", "lldt", "sgdt", "sidt", "wrmsr",
                      "clflush", "clflushopt", "clwb", "wbinvd", "invd", "swapgs", "sysret",
                      "sysretq", "iret", "iretq", "sysexit", "pushf", "pushfq", "out", "outb",
                      "outw", "outl", "endbr64", "monitor", "mwait", "wrfsbase", "wrgsbase",
                      "xsetbv", "fxsave", "fxrstor", "xsave", "xrstor", "xsaveopt", "fninit",
                      "ldmxcsr", "stmxcsr", "vzeroupper"})) {
        return;
    }
    if (m == "syscall") {
        x86_regs(out, {"rcx", "r11"});
        return;
    }
    if (m == "cpuid") {
        x86_regs(out, {"rax", "rbx", "rcx", "rdx"});
        return;
    }
    if (one_of(m, {"rdtsc", "rdmsr", "xgetbv", "rdpmc"})) {
        x86_regs(out, {"rax", "rdx"});
        return;
    }
    if (m == "rdtscp") {
        x86_regs(out, {"rax", "rdx", "rcx"});
        return;
    }
    if (one_of(m, {"cqto", "cqo", "cltd", "cdq", "cwtd", "cwd"})) {
        x86_regs(out, {"rdx"});
        return;
    }
    if (one_of(m, {"cltq", "cdqe", "cwtl", "cwde", "cbtw", "cbw"})) {
        x86_regs(out, {"rax"});
        return;
    }
    if (m == "leave" || m == "leaveq") {
        x86_regs(out, {"rbp"});
        return;
    }
    // One-operand multiply and divide: %rdx:%rax.
    if (x86_is(m, {"mul", "div", "idiv"}) || (x86_is(m, {"imul"}) && ops.size() == 1)) {
        x86_regs(out, {"rax", "rdx"});
        return;
    }
    if (one_of(m, {"cmpxchg8b", "cmpxchg16b"})) {
        x86_regs(out, {"rax", "rdx"});
        return;
    }
    if (x86_is(m, {"cmpxchg"})) {
        x86_regs(out, {"rax"});
        x86_add(out, last());
        return;
    }
    if (x86_is(m, {"xchg", "xadd"})) {
        for (auto o : ops) {
            x86_add(out, o);
        }
        return;
    }
    x86_add(out, last());  // the destination
}

}  // namespace

std::string x86_reg_key(std::string_view tok) {
    std::string t = lower(trim(tok));
    while (!t.empty() && t.front() == '%') {
        t.erase(0, 1);
    }
    static constexpr std::array<std::array<std::string_view, 5>, 7> legacy{{
        {"rax", "eax", "ax", "al", "ah"},
        {"rbx", "ebx", "bx", "bl", "bh"},
        {"rcx", "ecx", "cx", "cl", "ch"},
        {"rdx", "edx", "dx", "dl", "dh"},
        {"rsi", "esi", "si", "sil", ""},
        {"rdi", "edi", "di", "dil", ""},
        {"rbp", "ebp", "bp", "bpl", ""},
    }};
    for (const auto& row : legacy) {
        if (std::ranges::any_of(row, [&](std::string_view n) { return !n.empty() && t == n; })) {
            return std::string(row[0]);
        }
    }
    auto numbered = [&](std::string_view prefix, int hi, std::string_view suffixes) -> int {
        if (!std::string_view(t).starts_with(prefix)) {
            return -1;
        }
        std::string_view body = std::string_view(t).substr(prefix.size());
        if (!body.empty() && suffixes.find(body.back()) != std::string_view::npos) {
            body.remove_suffix(1);
        }
        if (!all_digits(body) || (body.size() > 1 && body.front() == '0')) {
            return -1;
        }
        const int n = std::stoi(std::string(body));
        return n <= hi ? n : -1;
    };
    if (const int n = numbered("r", 15, "dwb"); n >= 8) {
        return "r" + std::to_string(n);
    }
    for (std::string_view v : {"xmm", "ymm", "zmm"}) {
        if (const int n = numbered(v, 31, ""); n >= 0) {
            return "xmm" + std::to_string(n);
        }
    }
    if (const int n = numbered("k", 7, ""); n >= 0) {
        return "k" + std::to_string(n);
    }
    return {};
}

std::vector<AsmWrite> x86_line_writes(std::string_view line) {
    // A `#` comment ends the line; `;` separates statements.
    std::vector<AsmWrite> out;
    each_statement(line, "#", [&](std::string_view st) { x86_statement(st, out, false); });
    return out;
}

// ----- riscv64 --------------------------------------------------------------

namespace {

constexpr std::array<std::string_view, 32> kRvInt{
    "zero", "ra", "sp", "gp", "tp", "t0", "t1", "t2", "s0", "s1", "a0",
    "a1",   "a2", "a3", "a4", "a5", "a6", "a7", "s2", "s3", "s4", "s5",
    "s6",   "s7", "s8", "s9", "s10", "s11", "t3", "t4", "t5", "t6"};
constexpr std::array<std::string_view, 32> kRvFloat{
    "ft0", "ft1", "ft2",  "ft3",  "ft4", "ft5", "ft6", "ft7", "fs0", "fs1", "fa0",
    "fa1", "fa2", "fa3",  "fa4",  "fa5", "fa6", "fa7", "fs2", "fs3", "fs4", "fs5",
    "fs6", "fs7", "fs8",  "fs9",  "fs10", "fs11", "ft8", "ft9", "ft10", "ft11"};

void rv_add(std::vector<AsmWrite>& out, std::string_view op) {
    auto key = rv_reg_key(op);
    if (!key.empty()) {
        out.push_back({AsmWrite::Kind::Register, std::move(key)});
    }
}

void rv_statement(std::string_view stmt, std::vector<AsmWrite>& out) {
    stmt = strip_label(stmt);
    if (stmt.empty() || stmt.front() == '.') {
        return;
    }
    const auto sp = stmt.find_first_of(" \t");
    std::string m = lower(stmt.substr(0, sp));
    if (m.starts_with("c.")) {
        m.erase(0, 2);  // a compressed form writes what its full form does
    }
    const auto ops = sp == std::string_view::npos ? std::vector<std::string_view>{}
                                                  : split_operands(stmt.substr(sp));
    auto op = [&](std::size_t i) { return i < ops.size() ? ops[i] : std::string_view{}; };

    // Stores, branches, fences, traps and the rd-less CSR forms write no
    // integer or float register.
    if (one_of(m, {"sb", "sh", "sw", "sd", "fsh", "fsw", "fsd", "fsq", "swsp", "sdsp", "fsdsp",
                   "beq", "bne", "blt", "bge", "bltu", "bgeu", "beqz", "bnez", "blez", "bgez",
                   "bltz", "bgtz", "bgt", "ble", "bgtu", "bleu", "j", "jr", "ret", "nop", "fence",
                   "fence.i", "fence.tso", "sfence.vma", "hfence.vvma", "hfence.gvma", "ecall",
                   "ebreak", "wfi", "mret", "sret", "uret", "csrw", "csrs", "csrc", "csrwi",
                   "csrsi", "csrci", "pause", "unimp"})
        || m.starts_with("cbo.") || m.starts_with("prefetch.")) {
        return;
    }
    if (m == "jal" || m == "jalr") {
        rv_add(out, ops.size() <= 1 ? std::string_view("ra") : op(0));
        return;
    }
    if (m == "call") {
        rv_add(out, "ra");
        return;
    }
    if (m == "tail") {
        rv_add(out, "t1");
        return;
    }
    rv_add(out, op(0));  // rd
}

}  // namespace

std::string rv_reg_key(std::string_view tok) {
    const std::string t = lower(trim(tok));
    if (t == "fp") {
        return "s0";
    }
    auto index = [&](char prefix) -> int {
        if (t.size() < 2 || t.front() != prefix) {
            return -1;
        }
        const std::string_view body = std::string_view(t).substr(1);
        if (!all_digits(body) || (body.size() > 1 && body.front() == '0')) {
            return -1;
        }
        const int n = std::stoi(std::string(body));
        return n <= 31 ? n : -1;
    };
    std::string key;
    if (const int n = index('x'); n >= 0) {
        key = kRvInt[static_cast<std::size_t>(n)];
    } else if (const int f = index('f'); f >= 0) {
        key = kRvFloat[static_cast<std::size_t>(f)];
    } else if (std::ranges::find(kRvInt, t) != kRvInt.end()
               || std::ranges::find(kRvFloat, t) != kRvFloat.end()) {
        key = t;
    }
    if (key == "zero" || key == "sp") {
        return {};
    }
    return key;
}

std::vector<AsmWrite> rv_line_writes(std::string_view line) {
    // A `#` comment ends the line; `;` separates statements.
    std::vector<AsmWrite> out;
    each_statement(line, "#", [&](std::string_view st) { rv_statement(st, out); });
    return out;
}

std::vector<AsmWrite> asm_line_writes(std::string_view arch, std::string_view line) {
    if (arch == "aarch64") {
        return a64_line_writes(line);
    }
    if (arch == "x86_64") {
        return x86_line_writes(line);
    }
    if (arch == "riscv64") {
        return rv_line_writes(line);
    }
    return {};
}

std::string asm_reg_key(std::string_view arch, std::string_view tok) {
    if (arch == "aarch64") {
        return a64_reg_key(tok);
    }
    if (arch == "x86_64") {
        return x86_reg_key(tok);
    }
    if (arch == "riscv64") {
        return rv_reg_key(tok);
    }
    return {};
}

}  // namespace vestra::sema
