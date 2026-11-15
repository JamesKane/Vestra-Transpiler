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

// Split operands at top-level commas (not inside `[...]` or `{...}`).
std::vector<std::string_view> split_operands(std::string_view s) {
    std::vector<std::string_view> out;
    int depth = 0;
    std::size_t start = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '[' || c == '{') {
            ++depth;
        } else if (c == ']' || c == '}') {
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

void one_statement(std::string_view stmt, std::vector<AsmWrite>& out) {
    stmt = trim(stmt);
    // A leading label: `1:` or `name:`.
    if (const auto colon = stmt.find(':'); colon != std::string_view::npos) {
        const auto head = stmt.substr(0, colon);
        if (!head.empty() && std::ranges::all_of(head, [](char c) {
                return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '.';
            })) {
            stmt = trim(stmt.substr(colon + 1));
        }
    }
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
    if (const auto c = line.find("//"); c != std::string_view::npos) {
        line = line.substr(0, c);
    }
    std::vector<AsmWrite> out;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= line.size(); ++i) {
        if (i == line.size() || line[i] == ';') {
            one_statement(line.substr(start, i - start), out);
            start = i + 1;
        }
    }
    return out;
}

}  // namespace vestra::sema
