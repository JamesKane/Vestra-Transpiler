// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#include "vestra/diag/diagnostic.hpp"
#include "vestra/diag/source_manager.hpp"
#include "vestra/lex/lexer.hpp"
#include "vestra/parse/parser.hpp"
#include "vestra/sema/capability.hpp"
#include "vestra/sema/exclusivity.hpp"
#include "vestra/sema/ownership.hpp"
#include "vestra/sema/resolver.hpp"
#include "vestra/sema/types.hpp"

#include <doctest/doctest.h>

#include <string>

namespace {

struct CheckResult {
    std::size_t error_count = 0;
    std::string first_message;
};

// Full pipeline: lex → parse → resolver → ownership → exclusivity →
// capability. Each layer must be clean before we run the next, otherwise
// noise from one obscures the layer under test.
CheckResult check(std::string source) {
    vestra::diag::SourceManager sm;
    vestra::diag::DiagnosticReporter rep(sm);
    auto fid = sm.add_in_memory("<test>", std::move(source));
    vestra::lex::Lexer lex(sm, fid, rep);
    auto tokens = lex.tokenize();
    vestra::parse::Parser p(tokens, rep);
    auto unit = p.parse_unit();
    if (!rep.has_errors()) {
        vestra::sema::TypeArena arena;
        vestra::sema::Resolver res(unit, arena, rep);
        res.resolve();
        if (!rep.has_errors()) {
            vestra::sema::OwnershipChecker own(unit, res.resolution(), rep);
            own.check();
            vestra::sema::ExclusivityChecker excl(unit, res.resolution(), rep);
            excl.check();
            vestra::sema::CapabilityChecker cap(unit, res.resolution(), rep);
            cap.check();
        }
    }
    CheckResult cr;
    cr.error_count = rep.error_count();
    if (!rep.diagnostics().empty()) {
        cr.first_message = rep.diagnostics().front().message;
    }
    return cr;
}

}  // namespace

// ---- clean propagation -----------------------------------------------------

TEST_CASE("a function with `using Alloc` can call one that also declares it") {
    CHECK(check("func leaf() using Alloc {}\n"
                "func mid() using Alloc { leaf() }\n")
              .error_count
          == 0);
}

TEST_CASE("calling a non-using function from a using one is fine") {
    CHECK(check("func leaf() {}\n"
                "func mid() using Alloc { leaf() }\n")
              .error_count
          == 0);
}

// ---- missing capability at call site ---------------------------------------

TEST_CASE("calling `using Alloc` from a function without it is rejected") {
    auto r = check("func leaf() using Alloc {}\n"
                   "func bad() { leaf() }\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("missing capability 'Alloc'") != std::string::npos);
}

TEST_CASE("calling a multi-cap function flags each missing one") {
    auto r = check("func leaf() using Alloc, Log {}\n"
                   "func bad() { leaf() }\n");
    // The first error should be one of {Alloc, Log}; both should be reported.
    CHECK(r.error_count >= 2);
}

// ---- `with` blocks satisfy a capability ------------------------------------

TEST_CASE("a `with` block supplies the capability for its body") {
    CHECK(check("func leaf() using Alloc {}\n"
                "func make_alloc() -> Int32 { return 0 }\n"
                "func ok() {\n"
                "    with Alloc = make_alloc() {\n"
                "        leaf()\n"
                "    }\n"
                "}\n")
              .error_count
          == 0);
}

TEST_CASE("marker `with Asm { ... }` (no value) is fine") {
    CHECK(check("func raw() using Asm {}\n"
                "func ok() {\n"
                "    with Asm {\n"
                "        raw()\n"
                "    }\n"
                "}\n")
              .error_count
          == 0);
}

TEST_CASE("a `with` block does not leak outside its body") {
    auto r = check("func leaf() using Alloc {}\n"
                   "func bad() {\n"
                   "    with Alloc = 0 {\n"
                   "        leaf()\n"
                   "    }\n"
                   "    leaf()\n"
                   "}\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("missing capability 'Alloc'") != std::string::npos);
}

// ---- async + await + spawn -------------------------------------------------

TEST_CASE("an async function may await") {
    CHECK(check("async func leaf() -> Int32 { return 1 }\n"
                "async func mid() -> Int32 { return await leaf() }\n")
              .error_count
          == 0);
}

TEST_CASE("an async throws function with a propagating try-await checks clean") {
    // `await f(x)` of an async throws fn yields Result<Int32, E>; the `try`
    // unwraps it (propagating the error) in g's own throws context.
    CHECK(check("enum E { case bad }\n"
                "async func f(_ x: Int32) throws(E) -> Int32 {\n"
                "    if x < 0 { throw E.bad }\n"
                "    return x + 1\n"
                "}\n"
                "async func g(_ x: Int32) throws(E) -> Int32 {\n"
                "    let a = try await f(x)\n"
                "    return a + 1\n"
                "}\n")
              .error_count
          == 0);
}

TEST_CASE("spawning a void async fn and awaiting it checks clean") {
    // `spawn poke(x)` is a Future[Unit]; awaiting it yields Unit (discarded).
    CHECK(check("async func poke(_ x: Int32) { let y = x }\n"
                "async func run(_ x: Int32) -> Int32 {\n"
                "    let f = spawn poke(x)\n"
                "    await f\n"
                "    return x + 1\n"
                "}\n")
              .error_count
          == 0);
}

TEST_CASE("spawn yields a Future that await unwraps to the inner type") {
    // `spawn leaf()` is a Future[Int32]; awaiting it gives back the Int32,
    // so the addition and the Int32 return both type-check.
    CHECK(check("async func leaf(_ x: Int32) -> Int32 { return x + 1 }\n"
                "async func use() -> Int32 {\n"
                "    let f: Future[Int32] = spawn leaf(1)\n"
                "    let g = spawn leaf(2)\n"
                "    return await f + await g\n"
                "}\n")
              .error_count
          == 0);
}

TEST_CASE("await outside an async context is rejected") {
    auto r = check("async func leaf() -> Int32 { return 1 }\n"
                   "func bad() -> Int32 { return await leaf() }\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("missing capability 'Async'") != std::string::npos);
}

TEST_CASE("spawn outside an async context is rejected") {
    auto r = check("async func leaf() -> Int32 { return 1 }\n"
                   "func bad() {\n"
                   "    spawn leaf()\n"
                   "}\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("missing capability 'Async'") != std::string::npos);
}

TEST_CASE("non-async caller of an async function is rejected") {
    // Calling an async function (which has `using Async`) requires Async
    // to be in scope at the call site, even without await.
    auto r = check("async func leaf() {}\n"
                   "func bad() { leaf() }\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("missing capability 'Async'") != std::string::npos);
}

// ---- §10 Box.new requires Alloc -------------------------------------------

TEST_CASE("Box.new in a `using Alloc` fn is clean") {
    CHECK(check("func mk(_ x: Int32) using Alloc -> Box[Int32] { return Box.new(x) }\n").error_count
          == 0);
}

TEST_CASE("Box.new without `using Alloc` is reported by the capability checker") {
    auto r = check("func bad(_ x: Int32) -> Box[Int32] { return Box.new(x) }\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("missing capability 'Alloc'") != std::string::npos);
}

// ---- §A3 raw-mint primitives require RawMemory ----------------------------

TEST_CASE("MutPtr.unchecked outside RawMemory is rejected") {
    auto r = check("func bad(_ addr: UInt64) {\n"
                   "    let p: MutPtr[UInt32] = MutPtr.unchecked(fromAddress: addr)\n"
                   "}\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("missing capability 'RawMemory'") != std::string::npos);
}

TEST_CASE("Ptr.unchecked / Span.raw / MutSpan.raw under `with RawMemory` are clean") {
    CHECK(check("func ok(_ addr: UInt64) {\n"
                "    with RawMemory {\n"
                "        let p: MutPtr[UInt32] = MutPtr.unchecked(fromAddress: addr)\n"
                "        let s = MutSpan.raw(at: p, count: 4)\n"
                "        s[0] = 42\n"
                "    }\n"
                "}\n")
              .error_count
          == 0);
}

TEST_CASE("Sysreg.<name>.read/.write without Asm is rejected") {
    // §14.12 — sysreg access discharges the Asm capability at the
    // call site, parallel to how MmioView.at discharges Mmio.
    auto r = check("func bad() -> UInt64 {\n"
                   "    return Sysreg.midr_el1.read()\n"
                   "}\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("missing capability 'Asm'") != std::string::npos);
}

TEST_CASE("PerCpu.new without Alloc is rejected") {
    // §A11 (§14.8) the heap factory needs Alloc just like Box.new.
    auto r = check("func bad() -> UInt32 {\n"
                   "    let pc: Box[PerCpu[UInt32]] = PerCpu.new(0)\n"
                   "    return pc.value.mine()\n"
                   "}\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("missing capability 'Alloc'") != std::string::npos);
}

TEST_CASE("PerCpu.slot without RawMemory is rejected") {
    // §A11 (§14.8) the cross-hart accessor needs RawMemory because the
    // Ptr[T] result aliases another hart's storage without the
    // borrow-tracking .mine() carries.
    auto r = check("@noinit static c: PerCpu[UInt32]\n"
                   "func bad(_ h: UInt16) -> UInt32 {\n"
                   "    let p: Ptr[UInt32] = c.slot(h)\n"
                   "    return 0\n"
                   "}\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("missing capability 'RawMemory'") != std::string::npos);
}

TEST_CASE("memcpy / memset / memmove without RawMemory are rejected") {
    // §A10 (§15.4) the byte-range intrinsics are gated on RawMemory
    // the same way the §A5 cache ops and §A3 raw mints are.
    auto r = check("func bad(_ d_addr: UInt64) {\n"
                   "    var d: MutPtr[UInt8] = MutPtr.unchecked(fromAddress: 0)\n"
                   "    with RawMemory {\n"
                   "        d = MutPtr.unchecked(fromAddress: d_addr)\n"
                   "    }\n"
                   "    memset(d, 0, 1)\n"
                   "}\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("missing capability 'RawMemory'") != std::string::npos);
}

TEST_CASE("cleanData without RawMemory is rejected") {
    // §A5 (§14.10.3) data-cache ops are gated on RawMemory the same
    // way the §A3 raw mints are.
    auto r = check("func bad(_ addr: UInt64, _ n: Int) {\n"
                   "    var p: Ptr[UInt8] = Ptr.unchecked(fromAddress: 0)\n"
                   "    with RawMemory {\n"
                   "        p = Ptr.unchecked(fromAddress: addr)\n"
                   "    }\n"
                   "    let s = Span.raw(at: p, count: n)\n"  // already RawMemory-gated
                   "    cleanData(s)\n"                       // and so is this
                   "}\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("missing capability 'RawMemory'") != std::string::npos);
}

TEST_CASE("MmioView.at outside Mmio is rejected") {
    // §A6 (§14.11) view constructors require the Mmio capability.
    // The MutPtr mint already needs RawMemory; the view construction
    // needs Mmio on top — the audit lists both discharges.
    auto r = check("func bad(_ addr: UInt64) {\n"
                   "    with RawMemory {\n"
                   "        let p: MutPtr[UInt32] = MutPtr.unchecked(fromAddress: addr)\n"
                   "        let v = MmioView.at(p)\n"
                   "    }\n"
                   "}\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("missing capability 'Mmio'") != std::string::npos);
}

TEST_CASE("MmioRegion.at + MmioView.at under `with RawMemory { with Mmio { ... } }` are clean") {
    CHECK(check("func ok(_ addr: UInt64) {\n"
                "    with RawMemory {\n"
                "        with Mmio {\n"
                "            let p: MutPtr[UInt32] = MutPtr.unchecked(fromAddress: addr)\n"
                "            let v = MmioView.at(p)\n"
                "            let r = MmioRegion.at(p, 4)\n"
                "            v.write(42)\n"
                "        }\n"
                "    }\n"
                "}\n")
              .error_count
          == 0);
}

TEST_CASE("MmioWireView.at outside Mmio is rejected") {
    auto r = check("func bad(_ addr: UInt64) {\n"
                   "    with RawMemory {\n"
                   "        let p: MutPtr[UInt32] = MutPtr.unchecked(fromAddress: addr)\n"
                   "        let w = MmioWireView.at(p, .big)\n"
                   "    }\n"
                   "}\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("missing capability 'Mmio'") != std::string::npos);
}

// ---- §A7 (§14.13) InterruptsOff region rules ------------------------------

TEST_CASE("waitForInterrupt inside InterruptsOff is rejected") {
    auto r = check("func bad() {\n"
                   "    with InterruptsOff {\n"
                   "        waitForInterrupt()\n"
                   "    }\n"
                   "}\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("`waitForInterrupt` inside `with InterruptsOff`")
          != std::string::npos);
}

TEST_CASE("waitForEvent inside InterruptsOff is rejected") {
    auto r = check("func bad() {\n"
                   "    with InterruptsOff {\n"
                   "        waitForEvent()\n"
                   "    }\n"
                   "}\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("`waitForEvent` inside `with InterruptsOff`") != std::string::npos);
}

TEST_CASE("Cpu.relax / nop inside InterruptsOff is admitted") {
    // Rule 5 names the rejected ops explicitly; everything else
    // pipeline-related (relax / nop / prefetch) stays admitted.
    CHECK(check("func ok() {\n"
                "    with InterruptsOff {\n"
                "        relax()\n"
                "        nop()\n"
                "    }\n"
                "}\n")
              .error_count
          == 0);
}

TEST_CASE("calling a `using Alloc` function inside InterruptsOff is rejected") {
    auto r = check("func alloc_caller() using Alloc {}\n"
                   "func bad() using Alloc {\n"
                   "    with InterruptsOff {\n"
                   "        alloc_caller()\n"
                   "    }\n"
                   "}\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("`using Alloc` inside `with InterruptsOff`") != std::string::npos);
}

TEST_CASE("InterruptsOff bare scope with no rule violations is clean") {
    CHECK(check("func ok() {\n"
                "    with InterruptsOff {\n"
                "        let x: Int32 = 0\n"
                "    }\n"
                "}\n")
              .error_count
          == 0);
}

TEST_CASE("invalidateAllInstructions + tlb ops don't require RawMemory") {
    // I-cache global + TLB ops act on architectural state alone —
    // no memory range, no RawMemory gate.
    CHECK(check("func ok() {\n"
                "    invalidateAllInstructions()\n"
                "    tlbInvalidateAll(.innerShareable)\n"
                "    tlbInvalidatePage(0x1000, false, .currentEL)\n"
                "}\n")
              .error_count
          == 0);
}

TEST_CASE("Span.raw without RawMemory is rejected even with a valid Ptr in scope") {
    // Mint the pointer inside RawMemory but try to derive the span
    // outside — the cap check is per-call-site.
    auto r = check("func bad(_ addr: UInt64) {\n"
                   "    var p: MutPtr[UInt32] = MutPtr.unchecked(fromAddress: 0)\n"
                   "    with RawMemory {\n"
                   "        p = MutPtr.unchecked(fromAddress: addr)\n"
                   "    }\n"
                   "    let s = MutSpan.raw(at: p, count: 4)\n"
                   "}\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("missing capability 'RawMemory'") != std::string::npos);
}

// ---- existing examples remain clean ----------------------------------------

TEST_CASE("the shapes example checks clean under capabilities") {
    CHECK(check("module examples.shapes\n"
                "struct Point { var x: Int32; var y: Int32 }\n"
                "enum Color { case red; case green; case blue }\n"
                "func channel(_ c: Color) -> Int32 {\n"
                "    return match c { case .red: 1 case .green: 2 case .blue: 3 }\n"
                "}\n"
                "func sum_xy(_ p: Point) -> Int32 { return p.x + p.y }\n"
                "func compute() -> Int32 {\n"
                "    let p = Point(x: 10, y: 32)\n"
                "    let total: Int32 = sum_xy(p) + channel(Color.green)\n"
                "    return total\n"
                "}\n")
              .error_count
          == 0);
}

TEST_CASE("select binds a future arm's value and admits a default") {
    CHECK(check("async func leaf(_ x: Int32) -> Int32 { return x + 1 }\n"
                "async func pick() -> Int32 {\n"
                "    let a = spawn leaf(1)\n"
                "    let b = spawn leaf(2)\n"
                "    return select {\n"
                "        on let x = a: x\n"
                "        on let y = b: y\n"
                "        default:      0\n"
                "    }\n"
                "}\n")
              .error_count
          == 0);
}

TEST_CASE("a no-default channel select outside an async context is rejected") {
    // A blocking channel select co_awaits a SelectAwaiter, so it needs Async
    // in scope — the same gate as `await`. A non-async function lacks it.
    auto r = check("func bad(_ ch: Channel[Int32]) -> Int32 {\n"
                   "    return select {\n"
                   "        on let v = ch.receive(): v ?? -1\n"
                   "    }\n"
                   "}\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("Async") != std::string::npos);
}

TEST_CASE("a channel select with a default is allowed in a sync function") {
    // With a default the select polls (no co_await), so it needs no Async.
    CHECK(check("func poll(_ ch: Channel[Int32]) -> Int32 {\n"
                "    return select {\n"
                "        on let v = ch.receive(): v ?? -1\n"
                "        default: 0\n"
                "    }\n"
                "}\n")
              .error_count
          == 0);
}

TEST_CASE("a select event that is not a future is rejected") {
    auto r = check("async func f(_ n: Int32) -> Int32 {\n"
                   "    return select { on let x = n: x  default: 0 }\n"
                   "}\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("select event must be a Future[T]") != std::string::npos);
}

TEST_CASE("a blocking channel select with a timeout arm checks clean") {
    CHECK(check("async func pick(_ a: Channel[Int32], _ b: Channel[Int32]) -> Int32 {\n"
                "    return select {\n"
                "        on let av = a.receive(): av ?? -1\n"
                "        on let bv = b.receive(): bv ?? -2\n"
                "        timeout .milliseconds(100): -9\n"
                "    }\n"
                "}\n")
              .error_count
          == 0);
}

TEST_CASE("a timeout arm together with a default is rejected") {
    auto r = check("async func pick(_ a: Channel[Int32]) -> Int32 {\n"
                   "    return select {\n"
                   "        on let av = a.receive(): av ?? -1\n"
                   "        timeout .milliseconds(100): -9\n"
                   "        default: 0\n"
                   "    }\n"
                   "}\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("both a 'timeout' arm and a 'default'") != std::string::npos);
}

TEST_CASE("a mixed channel/future select checks clean") {
    CHECK(check("async func leaf(_ x: Int32) -> Int32 { return x + 1 }\n"
                "async func pick(_ ch: Channel[Int32]) -> Int32 {\n"
                "    let fut = spawn leaf(10)\n"
                "    return select {\n"
                "        on let v = ch.receive(): v ?? -1\n"
                "        on let r = fut: r\n"
                "    }\n"
                "}\n")
              .error_count
          == 0);
}

TEST_CASE("a timeout arm on a mixed select is rejected") {
    // A timeout needs the all-channel blocking form; a future arm disqualifies.
    auto r = check("async func leaf(_ x: Int32) -> Int32 { return x + 1 }\n"
                   "async func pick(_ ch: Channel[Int32]) -> Int32 {\n"
                   "    let fut = spawn leaf(10)\n"
                   "    return select {\n"
                   "        on let v = ch.receive(): v ?? -1\n"
                   "        on let r = fut: r\n"
                   "        timeout .milliseconds(100): -9\n"
                   "    }\n"
                   "}\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("'timeout' arm is only valid on a blocking select")
          != std::string::npos);
}

TEST_CASE("a timeout arm on a future select is rejected") {
    auto r = check("async func leaf(_ x: Int32) -> Int32 { return x + 1 }\n"
                   "async func pick() -> Int32 {\n"
                   "    let a = spawn leaf(1)\n"
                   "    return select {\n"
                   "        on let x = a: x\n"
                   "        timeout .milliseconds(100): -9\n"
                   "    }\n"
                   "}\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("'timeout' arm is only valid on a blocking select")
          != std::string::npos);
}

TEST_CASE("a non-Duration timeout delay is rejected") {
    // The delay used to be a bare ms integer; it is now a Duration.
    auto r = check("async func pick(_ a: Channel[Int32]) -> Int32 {\n"
                   "    return select {\n"
                   "        on let av = a.receive(): av ?? -1\n"
                   "        timeout 100: -9\n"
                   "    }\n"
                   "}\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("timeout delay must be a Duration") != std::string::npos);
}

TEST_CASE("Duration arithmetic types Swift-like — ratio is Float64, sum is Duration") {
    // `.seconds(n)` leading-dot factory, the explicit `Duration.milliseconds(n)`
    // form, `Duration / Duration -> Float64`, and `Duration + Duration ->
    // Duration` all check clean.
    CHECK(check("func ratio() -> Float64 {\n"
                "    let a: Duration = .seconds(10)\n"
                "    let b: Duration = .seconds(2)\n"
                "    return a / b\n"
                "}\n"
                "func total() -> Duration {\n"
                "    let a = Duration.milliseconds(500)\n"
                "    return a + a\n"
                "}\n")
              .error_count
          == 0);
}

TEST_CASE("Duration mixed with a non-Duration operand is rejected") {
    auto r = check("func bad() -> Duration {\n"
                   "    let a: Duration = .seconds(1)\n"
                   "    let n: Int32 = 1\n"
                   "    return a + n\n"
                   "}\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("requires both operands to be Duration") != std::string::npos);
}

TEST_CASE("an unknown Duration factory is rejected") {
    auto r = check("func bad() -> Duration {\n"
                   "    let d: Duration = .fortnights(2)\n"
                   "    return d\n"
                   "}\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("unknown Duration factory") != std::string::npos);
}

TEST_CASE("Duration scalar scaling types as Duration") {
    // Duration * Int, Int * Duration, and Duration / Int all yield a Duration;
    // a Duration / Duration ratio in the same function still types as Float64.
    CHECK(check("func scaled() -> Duration {\n"
                "    let q: Duration = .milliseconds(250)\n"
                "    let n: Int32 = 4\n"
                "    let a = q * n\n"
                "    let b = n * q\n"
                "    return (a + b) / 2\n"
                "}\n"
                "func ratio() -> Float64 {\n"
                "    let q: Duration = .seconds(2)\n"
                "    return (q * 3) / q\n"
                "}\n")
              .error_count
          == 0);
}

TEST_CASE("multiplying two Durations is rejected") {
    auto r = check("func bad() -> Duration {\n"
                   "    let a: Duration = .seconds(1)\n"
                   "    let b: Duration = .seconds(2)\n"
                   "    return a * b\n"
                   "}\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("cannot multiply two Durations") != std::string::npos);
}

TEST_CASE("Duration fractional scaling types as Duration") {
    // Duration * Float, Float * Duration, and Duration / Float all yield a
    // Duration (alongside the Int forms and the Duration / Duration ratio).
    CHECK(check("func scaled() -> Duration {\n"
                "    let one: Duration = .seconds(1)\n"
                "    let a = one * 1.5\n"
                "    let b = 2.0 * a\n"
                "    return b / 1.5\n"
                "}\n")
              .error_count
          == 0);
}

TEST_CASE("scaling a Duration by a non-numeric value is rejected") {
    auto r = check("func bad() -> Duration {\n"
                   "    let a: Duration = .seconds(1)\n"
                   "    return a * true\n"
                   "}\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("scaled by an integer or floating-point factor")
          != std::string::npos);
}

TEST_CASE("Duration unit accessors type as Int") {
    // `.nanoseconds` / `.microseconds` / `.milliseconds` / `.seconds` read the
    // whole count in that unit (accessed, not called — distinct from the
    // same-named factory).
    CHECK(check("func a() -> Int {\n"
                "    let d: Duration = .seconds(2)\n"
                "    return d.milliseconds + d.seconds + d.microseconds + d.nanoseconds\n"
                "}\n")
              .error_count
          == 0);
}

// ---- §11.2 parallel --------------------------------------------------------

TEST_CASE("parallel over a MutSpan with a worker closure checks clean") {
    CHECK(check("func bump(_ data: MutSpan[Int32], _ n: Int) using Async {\n"
                "    parallel(data, n, { slice =>\n"
                "        var i = 0\n"
                "        while i < slice.count { slice[i] = slice[i] + 1  i = i + 1 }\n"
                "    })\n"
                "}\n")
              .error_count
          == 0);
}

TEST_CASE("parallel without the Async capability is rejected") {
    auto r = check("func bump(_ data: MutSpan[Int32], _ n: Int) {\n"
                   "    parallel(data, n, { slice =>\n"
                   "        var i = 0\n"
                   "        while i < slice.count { slice[i] = slice[i] + 1  i = i + 1 }\n"
                   "    })\n"
                   "}\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("Async") != std::string::npos);
}

TEST_CASE("parallel rejects a non-MutSpan first argument") {
    auto r = check("func bad(_ n: Int) {\n"
                   "    parallel(n, 2, { slice => slice })\n"
                   "}\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("parallel's first argument must be a MutSpan[T]")
          != std::string::npos);
}

// ---- §11 Channel[T] --------------------------------------------------------

TEST_CASE("channel send / recv type-check; recv yields an Optional") {
    CHECK(check("func rt() -> Int32 {\n"
                "    let ch: Channel[Int32] = Channel.new()\n"
                "    ch.send(10)\n"
                "    return ch.recv() ?? 0\n"
                "}\n")
              .error_count
          == 0);
}

TEST_CASE("Channel.new without a contextual type is rejected") {
    auto r = check("func bad() {\n"
                   "    let ch = Channel.new()\n"
                   "}\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("cannot infer Channel element type") != std::string::npos);
}
