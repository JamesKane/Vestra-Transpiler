# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 James Kane

# Test target. Bundles all unit tests into one executable so we pay the
# doctest registration overhead only once.

set(test_sources
    lex/lexer_test.cpp
    parse/parser_test.cpp
    parse/printer_test.cpp
    sema/resolver_test.cpp
    sema/ownership_test.cpp
    sema/exclusivity_test.cpp
    sema/capability_test.cpp
    sema/generics_test.cpp
    sema/comptime_test.cpp
    codegen/cpp_emitter_test.cpp)

add_executable(vestra_tests
    main.cpp
    ${test_sources})

target_link_libraries(vestra_tests
    PRIVATE
        vestra_core
        doctest::doctest)

target_compile_features(vestra_tests PRIVATE cxx_std_26)
vestra_apply_warnings(vestra_tests)
vestra_apply_sanitizers(vestra_tests)

# Discover individual TEST_CASEs as separate CTest tests so they can be
# selected, parallelized, and re-run on failure.
include(${doctest_SOURCE_DIR}/scripts/cmake/doctest.cmake)
doctest_discover_tests(vestra_tests)

# ----------------------------------------------------------------------------
# End-to-end integration test: transpile examples/hello.vst, compile the
# resulting C++23 with the host compiler, run it, and check the exit status.
# This is the closest analog to "the language works."

set(e2e_workdir   ${CMAKE_BINARY_DIR}/_e2e)
set(e2e_input     ${CMAKE_SOURCE_DIR}/examples/hello.vst)
set(e2e_driver    ${CMAKE_SOURCE_DIR}/tests/e2e/main_hello.cpp)

add_test(
    NAME    e2e_hello_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_hello_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_driver} ${e2e_workdir}/hello.cpp
            -o ${e2e_workdir}/hello_app)
set_tests_properties(e2e_hello_compile PROPERTIES DEPENDS e2e_hello_transpile)

add_test(
    NAME    e2e_hello_run
    COMMAND ${e2e_workdir}/hello_app)
set_tests_properties(e2e_hello_run PROPERTIES
    DEPENDS                e2e_hello_compile
    PASS_REGULAR_EXPRESSION "compute\\(\\) = 1774")

# ---------------------------------------------------------------------------
# Dog-food compiler, phase 1: the Vestra lexer written in Vestra (selfhost/
# lexer.vst). It targets the real TokenKind set and 54 keywords, and must
# reproduce the C++ lexer's `--dump-tokens` output byte-for-byte. Validation is
# DIFFERENTIAL: transpile the Vestra lexer, compile it, then diff its token
# stream against the C++ lexer (its dump minus the trailing "wrote ..." lines)
# on a torture sample and on the lexer's own source (true self-tokenization).
# String interpolation is the one deferred gap, so the inputs avoid `\(`.
# ---------------------------------------------------------------------------
set(selfhost_lexer ${CMAKE_SOURCE_DIR}/selfhost/lexer.vst)
set(lex_sample     ${CMAKE_SOURCE_DIR}/tests/e2e/lex_sample.vst)
set(lex_dumpdir    ${e2e_workdir}/_lexdump)

add_test(
    NAME    e2e_selfhost_lexer_transpile
    COMMAND $<TARGET_FILE:vestra> build ${selfhost_lexer} -o ${e2e_workdir})

add_test(
    NAME    e2e_selfhost_lexer_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_workdir}/lexer.cpp
            -o ${e2e_workdir}/vlex)
set_tests_properties(e2e_selfhost_lexer_compile PROPERTIES DEPENDS e2e_selfhost_lexer_transpile)

# Differential: the self-hosted lexer matches the C++ lexer on the torture sample.
add_test(
    NAME    e2e_selfhost_lexer_diff_sample
    COMMAND sh -c "mkdir -p ${lex_dumpdir} && $<TARGET_FILE:vestra> build ${lex_sample} --dump-tokens -o ${lex_dumpdir} --skip-check 2>/dev/null | grep -v '^wrote ' > ${lex_dumpdir}/sample_c.txt && ${e2e_workdir}/vlex ${lex_sample} > ${lex_dumpdir}/sample_v.txt && diff ${lex_dumpdir}/sample_c.txt ${lex_dumpdir}/sample_v.txt")
set_tests_properties(e2e_selfhost_lexer_diff_sample PROPERTIES DEPENDS e2e_selfhost_lexer_compile)

# Differential: the lexer tokenizes its own source identically (dog-food proof).
add_test(
    NAME    e2e_selfhost_lexer_diff_self
    COMMAND sh -c "mkdir -p ${lex_dumpdir} && $<TARGET_FILE:vestra> build ${selfhost_lexer} --dump-tokens -o ${lex_dumpdir} --skip-check 2>/dev/null | grep -v '^wrote ' > ${lex_dumpdir}/self_c.txt && ${e2e_workdir}/vlex ${selfhost_lexer} > ${lex_dumpdir}/self_v.txt && diff ${lex_dumpdir}/self_c.txt ${lex_dumpdir}/self_v.txt")
set_tests_properties(e2e_selfhost_lexer_diff_self PROPERTIES DEPENDS e2e_selfhost_lexer_compile)

# ---------------------------------------------------------------------------
# Dog-food compiler, phase 2: the Vestra parser written in Vestra (selfhost/
# parser.vst). It reuses the self-hosted lexer, parses the token stream into an
# AST, and re-prints it in the C++ parser's canonical `--dump-ast` form. Slice 1
# covers func decls, let/var/return/assign/while statements, the full §17.7
# expression precedence ladder, and named/optional types; validation is
# DIFFERENTIAL against the C++ `--dump-ast` on a sample and on a real in-tree
# example file that falls within the slice-1 grammar.
# ---------------------------------------------------------------------------
set(selfhost_parser ${CMAKE_SOURCE_DIR}/selfhost/parser.vst)
set(parse_sample    ${CMAKE_SOURCE_DIR}/tests/e2e/parse_sample.vst)
set(parse_sample2   ${CMAKE_SOURCE_DIR}/tests/e2e/parse_sample2.vst)
set(parse_sample3   ${CMAKE_SOURCE_DIR}/tests/e2e/parse_sample3.vst)
set(parse_sample4   ${CMAKE_SOURCE_DIR}/tests/e2e/parse_sample4.vst)
set(parse_example   ${CMAKE_SOURCE_DIR}/examples/string_demo.vst)
set(parse_example2  ${CMAKE_SOURCE_DIR}/examples/shapes.vst)
set(parse_example3  ${CMAKE_SOURCE_DIR}/examples/simd_demo.vst)
set(parse_example4  ${CMAKE_SOURCE_DIR}/examples/result.vst)
set(parse_example5  ${CMAKE_SOURCE_DIR}/examples/layout.vst)
set(ast_dumpdir     ${e2e_workdir}/_astdump)

add_test(
    NAME    e2e_selfhost_parser_transpile
    COMMAND $<TARGET_FILE:vestra> build ${selfhost_parser} -o ${e2e_workdir})

add_test(
    NAME    e2e_selfhost_parser_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_workdir}/parser.cpp
            -o ${e2e_workdir}/vparse)
set_tests_properties(e2e_selfhost_parser_compile PROPERTIES DEPENDS e2e_selfhost_parser_transpile)

# Differential: the self-hosted parser's canonical AST matches the C++ parser on
# the slice-1 torture sample.
add_test(
    NAME    e2e_selfhost_parser_diff_sample
    COMMAND sh -c "mkdir -p ${ast_dumpdir} && $<TARGET_FILE:vestra> build ${parse_sample} --dump-ast -o ${ast_dumpdir} --skip-check 2>/dev/null | grep -v '^wrote ' > ${ast_dumpdir}/sample_c.txt && ${e2e_workdir}/vparse ${parse_sample} > ${ast_dumpdir}/sample_v.txt && diff ${ast_dumpdir}/sample_c.txt ${ast_dumpdir}/sample_v.txt")
set_tests_properties(e2e_selfhost_parser_diff_sample PROPERTIES DEPENDS e2e_selfhost_parser_compile)

# Differential: the struct / enum / match / for / pattern sample.
add_test(
    NAME    e2e_selfhost_parser_diff_sample2
    COMMAND sh -c "mkdir -p ${ast_dumpdir} && $<TARGET_FILE:vestra> build ${parse_sample2} --dump-ast -o ${ast_dumpdir} --skip-check 2>/dev/null | grep -v '^wrote ' > ${ast_dumpdir}/sample2_c.txt && ${e2e_workdir}/vparse ${parse_sample2} > ${ast_dumpdir}/sample2_v.txt && diff ${ast_dumpdir}/sample2_c.txt ${ast_dumpdir}/sample2_v.txt")
set_tests_properties(e2e_selfhost_parser_diff_sample2 PROPERTIES DEPENDS e2e_selfhost_parser_compile)

# Differential: real in-tree examples within the grammar — a string-library file
# (slice-1 expressions) and a struct + enum + match file (this slice).
add_test(
    NAME    e2e_selfhost_parser_diff_example
    COMMAND sh -c "mkdir -p ${ast_dumpdir} && $<TARGET_FILE:vestra> build ${parse_example} --dump-ast -o ${ast_dumpdir} --skip-check 2>/dev/null | grep -v '^wrote ' > ${ast_dumpdir}/ex_c.txt && ${e2e_workdir}/vparse ${parse_example} > ${ast_dumpdir}/ex_v.txt && diff ${ast_dumpdir}/ex_c.txt ${ast_dumpdir}/ex_v.txt")
set_tests_properties(e2e_selfhost_parser_diff_example PROPERTIES DEPENDS e2e_selfhost_parser_compile)

add_test(
    NAME    e2e_selfhost_parser_diff_example2
    COMMAND sh -c "mkdir -p ${ast_dumpdir} && $<TARGET_FILE:vestra> build ${parse_example2} --dump-ast -o ${ast_dumpdir} --skip-check 2>/dev/null | grep -v '^wrote ' > ${ast_dumpdir}/ex2_c.txt && ${e2e_workdir}/vparse ${parse_example2} > ${ast_dumpdir}/ex2_v.txt && diff ${ast_dumpdir}/ex2_c.txt ${ast_dumpdir}/ex2_v.txt")
set_tests_properties(e2e_selfhost_parser_diff_example2 PROPERTIES DEPENDS e2e_selfhost_parser_compile)

# Differential: the generics / tuple-vector-function types / const-static-opaque-
# derive / attributes sample, and a real vector-type example.
add_test(
    NAME    e2e_selfhost_parser_diff_sample3
    COMMAND sh -c "mkdir -p ${ast_dumpdir} && $<TARGET_FILE:vestra> build ${parse_sample3} --dump-ast -o ${ast_dumpdir} --skip-check 2>/dev/null | grep -v '^wrote ' > ${ast_dumpdir}/sample3_c.txt && ${e2e_workdir}/vparse ${parse_sample3} > ${ast_dumpdir}/sample3_v.txt && diff ${ast_dumpdir}/sample3_c.txt ${ast_dumpdir}/sample3_v.txt")
set_tests_properties(e2e_selfhost_parser_diff_sample3 PROPERTIES DEPENDS e2e_selfhost_parser_compile)

add_test(
    NAME    e2e_selfhost_parser_diff_example3
    COMMAND sh -c "mkdir -p ${ast_dumpdir} && $<TARGET_FILE:vestra> build ${parse_example3} --dump-ast -o ${ast_dumpdir} --skip-check 2>/dev/null | grep -v '^wrote ' > ${ast_dumpdir}/ex3_c.txt && ${e2e_workdir}/vparse ${parse_example3} > ${ast_dumpdir}/ex3_v.txt && diff ${ast_dumpdir}/ex3_c.txt ${ast_dumpdir}/ex3_v.txt")
set_tests_properties(e2e_selfhost_parser_diff_example3 PROPERTIES DEPENDS e2e_selfhost_parser_compile)

# Differential: the exotic-expression sample (copy / await / spawn / try / do-catch /
# throw / comptime / closure / select / where) and a real try / do-catch example.
add_test(
    NAME    e2e_selfhost_parser_diff_sample4
    COMMAND sh -c "mkdir -p ${ast_dumpdir} && $<TARGET_FILE:vestra> build ${parse_sample4} --dump-ast -o ${ast_dumpdir} --skip-check 2>/dev/null | grep -v '^wrote ' > ${ast_dumpdir}/sample4_c.txt && ${e2e_workdir}/vparse ${parse_sample4} > ${ast_dumpdir}/sample4_v.txt && diff ${ast_dumpdir}/sample4_c.txt ${ast_dumpdir}/sample4_v.txt")
set_tests_properties(e2e_selfhost_parser_diff_sample4 PROPERTIES DEPENDS e2e_selfhost_parser_compile)

add_test(
    NAME    e2e_selfhost_parser_diff_example4
    COMMAND sh -c "mkdir -p ${ast_dumpdir} && $<TARGET_FILE:vestra> build ${parse_example4} --dump-ast -o ${ast_dumpdir} --skip-check 2>/dev/null | grep -v '^wrote ' > ${ast_dumpdir}/ex4_c.txt && ${e2e_workdir}/vparse ${parse_example4} > ${ast_dumpdir}/ex4_v.txt && diff ${ast_dumpdir}/ex4_c.txt ${ast_dumpdir}/ex4_v.txt")
set_tests_properties(e2e_selfhost_parser_diff_example4 PROPERTIES DEPENDS e2e_selfhost_parser_compile)

# Differential: a real file with @bits(...) field attributes (dropped by the printer)
# and tuple-pattern parameters (printed `_ : T`).
add_test(
    NAME    e2e_selfhost_parser_diff_example5
    COMMAND sh -c "mkdir -p ${ast_dumpdir} && $<TARGET_FILE:vestra> build ${parse_example5} --dump-ast -o ${ast_dumpdir} --skip-check 2>/dev/null | grep -v '^wrote ' > ${ast_dumpdir}/ex5_c.txt && ${e2e_workdir}/vparse ${parse_example5} > ${ast_dumpdir}/ex5_v.txt && diff ${ast_dumpdir}/ex5_c.txt ${ast_dumpdir}/ex5_v.txt")
set_tests_properties(e2e_selfhost_parser_diff_example5 PROPERTIES DEPENDS e2e_selfhost_parser_compile)

# `while let` desugars to the same tree in both parsers (F18).
add_test(
    NAME    e2e_selfhost_parser_diff_whilelet
    COMMAND sh -c "mkdir -p ${ast_dumpdir} && $<TARGET_FILE:vestra> build ${CMAKE_SOURCE_DIR}/examples/match_flow_demo/match_flow_demo.vst --dump-ast -o ${ast_dumpdir} --skip-check 2>/dev/null | grep -v '^wrote ' > ${ast_dumpdir}/wl_c.txt && ${e2e_workdir}/vparse ${CMAKE_SOURCE_DIR}/examples/match_flow_demo/match_flow_demo.vst > ${ast_dumpdir}/wl_v.txt && diff ${ast_dumpdir}/wl_c.txt ${ast_dumpdir}/wl_v.txt")
set_tests_properties(e2e_selfhost_parser_diff_whilelet PROPERTIES DEPENDS e2e_selfhost_parser_compile)

# ---------------------------------------------------------------------------
# Dog-food compiler, phase 3: the Vestra semantic analyzer written in Vestra
# (selfhost/sema.vst). Slice 1 is name resolution (undefined identifiers); slice 2
# is capability checking (each capability use — print*/readFile/Vec.new/await/a
# called function's `using` row — must be satisfied by the function's `using` row
# or an enclosing `with` block); slice 3 is exclusivity (§5.4): within a call's
# argument list no two arguments may produce overlapping accesses that conflict
# (a place borrowed inout twice, or inout-and-read), plus the §11 rule that an
# `async func` may not take an `inout` parameter; slice 4 is ownership / move
# tracking (§5, §19.2): a non-trivial binding moved by a let/var-bind, a return,
# or a sink/constructor argument may not then be used (`use of '...' after it was
# moved`) or moved again (`'...' was already moved...`); slice 5 begins type
# checking (§17): a `let`/`var`/`const` whose initializer types as something not
# assignable to its annotation (`initializer of type X does not match annotation Y`
# / `const value of type X ...`), over a foundational inference subset (literals,
# identifiers, parens, direct calls) and assignability (identity, Int/UInt natural
# width, the StrConst<=Str<=String lattice, T->T?); slice 6 adds call checking — a
# call to a known function must have the right arity (`call expects N argument(s),
# got M`) and each argument assignable to its parameter (`argument i of type X does
# not match parameter type Y`); slice 7 adds return checking — a `return EXPR` must
# be assignable to the declared result (`return value of type X does not match
# function result type Y`) and a body's trailing expression must match it
# (`function '...' returns X but body produces Y`); slice 8 adds condition typing —
# an `if`/`while` condition must be Bool (`if/while condition must be Bool, got X`),
# which brings binary/unary inference (comparisons and logical ops type as Bool,
# arithmetic as the sized operand, `!` as Bool); slice 9 emits binary-operator
# errors — arithmetic needs numeric operands, bitwise needs integers, comparisons/
# arithmetic/bitwise need matching operand types, and `&&`/`||` need Bool operands;
# slice 10 checks assignment — the value must be assignable to the target slot
# (`cannot assign value of type X to target of type Y`); slice 11 checks struct
# construction — each labeled argument must name a field that accepts its value
# (`field '..' of type X cannot accept value of type Y`), unknown labels / missing
# fields / unlabeled args are reported too; slice 12 types member access (`s.field`
# -> the field's type, nested too), so member expressions now flow through every
# earlier check (initializer, argument, assignment, return) rather than typing as
# unknown — no new message family, just wider coverage; slice 13 types indexing —
# `v[i]` on a Vec/Span/MutSpan yields the element type (flowing through later
# checks), each index must be integer (`index must be integer-typed, got X`), and a
# known non-indexable base is `type X is not indexable`. Validation is DIFFERENTIAL against
# `vestra check`: each diff test extracts the `undefined name '...'`,
# `missing capability '...'`, borrow-conflict, async-inout, move-tracking,
# type-mismatch, call/argument, return-type, condition, binary-operator,
# assignment, and struct-construction messages from both and compares them.
# `vsema` mirrors `check`'s pass ORDER (type
# checking is part of resolution, so it gates like undefined names; then ownership,
# exclusivity, capability). Sema is cross-referential, so this runs on a CURATED
# corpus (otherwise-valid programs isolating one rule), not the whole example corpus.
# ---------------------------------------------------------------------------
set(selfhost_sema  ${CMAKE_SOURCE_DIR}/selfhost/sema.vst)
set(sema_dir       ${CMAKE_SOURCE_DIR}/tests/e2e/sema)
set(sema_workdir   ${e2e_workdir}/_sema)

add_test(
    NAME    e2e_selfhost_sema_transpile
    COMMAND $<TARGET_FILE:vestra> build ${selfhost_sema} -o ${e2e_workdir})

add_test(
    NAME    e2e_selfhost_sema_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_workdir}/sema.cpp
            -o ${e2e_workdir}/vsema)
set_tests_properties(e2e_selfhost_sema_compile PROPERTIES DEPENDS e2e_selfhost_sema_transpile)

# One differential test per curated program: the self-hosted analyzer's
# undefined-name + missing-capability + exclusivity + ownership + type-mismatch
# verdict must match `vestra check`. The async-inout pattern uses ERE bracket
# classes ([(] [)]) and `.` for the backticks so the regex survives the sh -c /
# CMake quoting unescaped.
set(sema_grep "undefined name '[^']*'|duplicate definition of '[^']*'|missing capability '[^']*'|'[^']*' is borrowed twice with incompatible modes|'[^']*' and '[^']*' overlap with incompatible borrow modes|async function '[^']*' cannot take an .inout. parameter [(]'[^']*'[)]|use of '[^']*' after it was moved|'[^']*' was already moved and cannot be moved again|initializer of type [^ ]+ does not match annotation [^ ]+|const value of type [^ ]+ does not match annotation [^ ]+|argument [0-9]+ of type [^ ]+ does not match parameter type [^ ]+|call expects [0-9]+ argument[(]s[)], got [0-9]+|return value of type [^ ]+ does not match function result type [^ ]+|function '[^']*' returns [^ ]+ but body produces [^ ]+|if condition must be Bool, got [^ ]+|while condition must be Bool, got [^ ]+|binary operator operands of different types: [^ ]+ vs [^ ]+|arithmetic operator requires numeric operands, got [^ ]+|bitwise operator requires integer operands, got [^ ]+|logical && / [|][|] require Bool operands|cannot assign value of type [^ ]+ to target of type [^ ]+|field '[^']*' of type [^ ]+ cannot accept value of type [^ ]+|'[^']*' has no field '[^']*'|struct '[^']*' constructor is missing field '[^']*'|struct constructor argument [0-9]+ requires a field label|index must be integer-typed, got [^ ]+|type [^ ]+ is not indexable|unary '-' requires a numeric type, got [^ ]+|unary '~' requires an integer type, got [^ ]+|enum '[^']*' has no case '[^']*'|'throw' is only valid inside a function declared 'throws[(]E[)]'|unknown type '[^']*'|'[^']*' is not a generic type|match guard must be Bool, got [^ ]+|match is not exhaustive: case '[^']*' is not covered|match arms have different types: [^ ]+ vs [^ ]+|enum pattern '[^']*' used against non-enum type [^ ]+|case '[^']*' has [0-9]+ payload field[(]s[)], pattern binds [0-9]+|literal of type [^ ]+ does not match scrutinee of type [^ ]+|cannot find imported module '[^']*' [(]searched: [^)]*[)]|module '[^']*' has no public member '[^']*'|module '[^']*' has no public type '[^']*'|'[^']*' is not an imported module|function '[^']*' returns Never but its body can complete normally|function '[^']*' can reach the end of its body without returning its [^ ]+ result|linear value '[^']*' is never consumed; a linear value must be moved|linear value '[^']*' is consumed on some branches but leaks|linear value '[^']*' leaks on this path, which leaves the function|linear value '[^']*' is never consumed; .if let. moved it out|assigning to '[^']*' drops the linear value it still holds|can't move the linear value[^;]*|assigning to '[^']*' could drop the linear value it holds|assigning to '[^']*' drops the linear value it holds|'[^']*' is borrowed from its place|'[^']*' is bound with .let., so it can't be changed|'[^']*' is a .read. parameter, so it can't be changed here|'[^']*' is a static, which holds an immutable value|'[^']*' is moved inside a loop, so the next iteration would use it after the move|use of '[^']*' after '[^']*' was moved out of it|can't move '[^']*' out of '[^']*': it's|can't .copy. a value of type [^:]*: it [a-z ]*|a closure captures by value, and '[^']*' is a .read. or .inout. parameter|linear value '[^']*' leaks if the .try. throws|the linear value this call returns [(][^)]*[)] is dropped|[^ ]+ would (put a non-escapable value on the heap|carry a non-escapable value|hold a Future)|field '[^']*' would store a Future|can't return a view of '[^']*': it's local to '[^']*'|can't return a view of a temporary|can't return a view minted under .with RawMemory.|can't return an MMIO view minted|'[^']*' can't return a Future|'[^']*' would outlive '[^']*', which its view comes from|'[^']*' would hold a view of a temporary|'[^']*' would outlive the .with (RawMemory|Mmio). block|'[^']*' is a view held across an .await.|cannot .spawn. a call with a [^ ]+ parameter|unknown capability '[^']*'|(argument [0-9]+|initializer) of type [(][^)]*[)][A-Za-z, ]* -> [^ ]+ does not match (parameter type|annotation) [(][^)]*[)][A-Za-z, ]* -> [^ ]+|.[a-zA-Z]+. inside .with InterruptsOff. is a deadlock|'[^']*' waits for an interrupt, so calling it inside .with InterruptsOff. is a deadlock|unknown attribute '@[^']*'|attribute '@[^']*' is not supported yet|unknown protocol '[^']*'|bound '[^']*' is not supported yet")
foreach(prog resolve_ok resolve_bad resolve_scope cap_ok cap_missing excl_ok excl_bad own_ok own_bad ty_ok ty_bad call_ok call_bad ret_ok ret_bad cond_ok cond_bad op_ok op_bad asg_ok asg_bad struct_ok struct_bad mem_ok mem_bad index_ok index_bad unwrap_ok unwrap_bad method_ok method_bad unop_ok unop_bad enum_ok enum_bad dup_ok dup_bad duploc_bad throw_ok throw_bad unktype_ok unktype_bad notgen_ok notgen_bad match_ok match_bad exhaust_ok exhaust_bad armty_ok armty_bad pat_ok patnon_bad patarity_bad modimp_ok modimp_bad modmem_ok modmem_bad modtyp_ok modtyp_bad notmod_bad veclit_ok veclit_bad tuplit_ok tuplit_bad clo_ok clo_bad clodup_bad interp_ok interp_bad arrlit_ok arrlit_bad arrnest_bad fwdtype_ok litleft_ok trailcomma_ok arrlitfit_ok diverge_ok diverge_bad linear_ok linear_bad linflow_ok linflow_bad linplace_ok linplace_bad mut_ok mut_bad staticmut_bad loopmove_ok loopmove_bad fieldmove_ok fieldmove_bad copy_ok copy_bad closure_ok closure_bad trylin_ok trylin_bad escslot_ok escslot_bad escret_ok escret_bad escstore_ok escstore_bad escasync_ok escasync_bad escspawn_bad capcall_ok capcall_bad capunknown_bad capfnval_ok capfnval_bad capfnrow_bad capwait_ok capwait_bad attr_ok attr_bad bound_ok bound_bad)
    add_test(
        NAME    e2e_selfhost_sema_diff_${prog}
        COMMAND sh -c "mkdir -p ${sema_workdir} && $<TARGET_FILE:vestra> check ${sema_dir}/${prog}.vst 2>&1 | grep -oE \"${sema_grep}\" > ${sema_workdir}/${prog}_c.txt; ${e2e_workdir}/vsema ${sema_dir}/${prog}.vst | grep -oE \"${sema_grep}\" > ${sema_workdir}/${prog}_v.txt; diff ${sema_workdir}/${prog}_c.txt ${sema_workdir}/${prog}_v.txt")
    set_tests_properties(e2e_selfhost_sema_diff_${prog} PROPERTIES DEPENDS e2e_selfhost_sema_compile)
endforeach()

# §15.5 the --no-libc hosted-feature rejections, both compilers given the flag.
add_test(
    NAME    e2e_selfhost_sema_diff_nolibc_bad
    COMMAND sh -c "mkdir -p ${sema_workdir} && $<TARGET_FILE:vestra> check ${sema_dir}/nolibc_bad.vst --no-libc 2>&1 | grep -oE '`[A-Za-z]*` needs the hosted runtime[^.]*' > ${sema_workdir}/nolibc_c.txt; ${e2e_workdir}/vsema --no-libc ${sema_dir}/nolibc_bad.vst | grep -oE '`[A-Za-z]*` needs the hosted runtime[^.]*' > ${sema_workdir}/nolibc_v.txt; test -s ${sema_workdir}/nolibc_c.txt && diff ${sema_workdir}/nolibc_c.txt ${sema_workdir}/nolibc_v.txt")
set_tests_properties(e2e_selfhost_sema_diff_nolibc_bad PROPERTIES DEPENDS e2e_selfhost_sema_compile)

# ---------------------------------------------------------------------------
# Self-hosted C++ code generator (selfhost/codegen.vst). It re-uses the lexer +
# parser, then emits the generated `.cpp` definitions to stdout. The differential
# runs `vestra build` to produce the reference <stem>.cpp and diffs it against
# `vgen`'s stdout, byte-for-byte. The fixed runtime prelude and forward
# declarations live in the `.hpp` (mirrored in a later slice); this checks the
# per-construct definitions codegen actually generates, on a CURATED corpus of
# valid programs (a build error would suppress the reference .cpp).
# ---------------------------------------------------------------------------
set(selfhost_codegen ${CMAKE_SOURCE_DIR}/selfhost/codegen.vst)
set(cg_dir           ${CMAKE_SOURCE_DIR}/tests/e2e/codegen)
set(cg_workdir       ${e2e_workdir}/_codegen)
# Every differential test builds into its OWN directory
# (<workdir>/<cpp|hpp>/<prog>). Under `ctest -j` a shared output dir raced:
# a fixture's .cpp and .hpp tests both run `vestra build` on it, and a fixture
# that imports another (cg_import -> cghelp) rewrites that module's outputs
# too, so a test could read a file another test was still writing.

add_test(
    NAME    e2e_selfhost_codegen_transpile
    COMMAND $<TARGET_FILE:vestra> build ${selfhost_codegen} -o ${e2e_workdir})

add_test(
    NAME    e2e_selfhost_codegen_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_workdir}/codegen.cpp
            -o ${e2e_workdir}/vgen)
set_tests_properties(e2e_selfhost_codegen_compile PROPERTIES DEPENDS e2e_selfhost_codegen_transpile)

foreach(prog cg_func cg_copy cg_linplace cg_expr cg_stmt cg_if cg_for cg_struct cg_index cgh_struct cgh_enum cgh_enumpl cg_enumcase cg_unwrap cg_match cg_enumctor cg_paymatch cg_matchdefault cg_matchguard cgh_enummethod cg_breakcont cgh_const cg_with cg_fnptr cg_closure cg_closurearg cg_matchpos cg_namedpl cg_print cg_mclosure cg_ifval cg_gpaymatch cg_builtins cg_lits cg_optchain cg_strco cg_interp cg_bmethods cg_genfunc cg_gentype cg_pmodes cg_throws cg_async cg_import cg_module cghelp cg_qualty cg_maperr cg_condhoist cg_matchhoist cg_docatch cg_gentail cg_amethod cg_channel cg_main cg_iflet cg_forproto cg_tpwild cg_paybindty cg_nestmatch cg_forpair cg_leaddot cg_valmatch cg_elseif cg_derive cg_derivespec cg_argsvec cg_opaque cg_box cg_moveonly cg_barecatch cg_hoistarms cg_tuplety cg_blockexpr cg_forelem cgh_global cg_orpat cg_withty cg_sinkarg cg_addrof cg_optflat cgh_requires cg_ifletval cgh_systypes cg_atomics cg_syncintr cg_sysstatic cg_linkattrs cgh_layout cgh_vectable cg_sysreg cg_rawmem cg_iteradapt cg_vecops cg_maperrclos cg_constgen cg_parallel cg_tupleparam cg_binprec cg_externabi cg_unitthrows)
    add_test(
        NAME    e2e_selfhost_codegen_diff_${prog}
        COMMAND sh -c "mkdir -p ${cg_workdir}/cpp/${prog} && $<TARGET_FILE:vestra> build ${cg_dir}/${prog}.vst -o ${cg_workdir}/cpp/${prog} >/dev/null 2>&1; ${e2e_workdir}/vgen ${cg_dir}/${prog}.vst > ${cg_workdir}/cpp/${prog}/${prog}_got.cpp; diff ${cg_workdir}/cpp/${prog}/${prog}.cpp ${cg_workdir}/cpp/${prog}/${prog}_got.cpp")
    set_tests_properties(e2e_selfhost_codegen_diff_${prog} PROPERTIES DEPENDS e2e_selfhost_codegen_compile)
endforeach()

# §5 cross-module enums and functions (F19): vgen loads the imported module
# (examples/modenum_demo/kern/ipc.vst) and lowers references to it as the
# bootstrap does, in the .cpp and the header tail.
set(cg_mod_dir ${cg_workdir}/modenum)
add_test(
    NAME    e2e_selfhost_codegen_diff_modenum
    COMMAND sh -c "rm -rf ${cg_mod_dir} && mkdir -p ${cg_mod_dir} && $<TARGET_FILE:vestra> build ${CMAKE_SOURCE_DIR}/examples/modenum_demo/main.vst -o ${cg_mod_dir} >/dev/null 2>&1; ${e2e_workdir}/vgen ${CMAKE_SOURCE_DIR}/examples/modenum_demo/main.vst > ${cg_mod_dir}/main_got.cpp; diff ${cg_mod_dir}/main.cpp ${cg_mod_dir}/main_got.cpp")
set_tests_properties(e2e_selfhost_codegen_diff_modenum PROPERTIES DEPENDS e2e_selfhost_codegen_compile)
add_test(
    NAME    e2e_selfhost_codegen_hpp_modenum
    COMMAND sh -c "rm -rf ${cg_mod_dir}_hpp && mkdir -p ${cg_mod_dir}_hpp && $<TARGET_FILE:vestra> build ${CMAKE_SOURCE_DIR}/examples/modenum_demo/main.vst -o ${cg_mod_dir}_hpp >/dev/null 2>&1; awk 'f{print} index(\$0, \"#endif  // VESTRA_RUNTIME_PRELUDE\"){f=1}' ${cg_mod_dir}_hpp/main.hpp > ${cg_mod_dir}_hpp/main_href.txt; ${e2e_workdir}/vgen --hpp ${CMAKE_SOURCE_DIR}/examples/modenum_demo/main.vst > ${cg_mod_dir}_hpp/main_hgot.txt; diff ${cg_mod_dir}_hpp/main_href.txt ${cg_mod_dir}_hpp/main_hgot.txt")
set_tests_properties(e2e_selfhost_codegen_hpp_modenum PROPERTIES DEPENDS e2e_selfhost_codegen_compile)

# §15.5 the freestanding profile: `vgen --no-libc` against `vestra build
# --no-libc` (the marker line, the link-time panic hook, the vector table).
set(cg_fs_dir ${cg_workdir}/fs)
add_test(
    NAME    e2e_selfhost_codegen_diff_vectable
    COMMAND sh -c "rm -rf ${cg_fs_dir} && mkdir -p ${cg_fs_dir} && $<TARGET_FILE:vestra> build ${cg_dir}/cgf_vectable.vst --no-libc -o ${cg_fs_dir} >/dev/null 2>&1; ${e2e_workdir}/vgen --no-libc ${cg_dir}/cgf_vectable.vst > ${cg_fs_dir}/vt_got.cpp; diff ${cg_fs_dir}/cgf_vectable.cpp ${cg_fs_dir}/vt_got.cpp")
set_tests_properties(e2e_selfhost_codegen_diff_vectable PROPERTIES DEPENDS e2e_selfhost_codegen_compile)
add_test(
    NAME    e2e_selfhost_codegen_hpp_vectable
    COMMAND sh -c "rm -rf ${cg_fs_dir}_hpp && mkdir -p ${cg_fs_dir}_hpp && $<TARGET_FILE:vestra> build ${cg_dir}/cgf_vectable.vst --no-libc -o ${cg_fs_dir}_hpp >/dev/null 2>&1; awk 'f{print} index(\$0, \"#endif  // VESTRA_RUNTIME_PRELUDE\"){f=1}' ${cg_fs_dir}_hpp/cgf_vectable.hpp > ${cg_fs_dir}_hpp/vt_href.txt; ${e2e_workdir}/vgen --hpp --no-libc ${cg_dir}/cgf_vectable.vst > ${cg_fs_dir}_hpp/vt_hgot.txt; diff ${cg_fs_dir}_hpp/vt_href.txt ${cg_fs_dir}_hpp/vt_hgot.txt")
set_tests_properties(e2e_selfhost_codegen_hpp_vectable PROPERTIES DEPENDS e2e_selfhost_codegen_compile)

# Header (`.hpp`) differential: `vgen --hpp` emits the per-program tail of the
# generated header (declarations after the fixed runtime prelude); the reference
# is the real .hpp with everything up to and including the
# `#endif // VESTRA_RUNTIME_PRELUDE` marker stripped (literal-substring awk match).
# Restricted to declaration sets vgen can already emit (free-function forward
# declarations); struct / enum definition emission grows this list.
foreach(prog cg_func cg_linplace cg_expr cg_stmt cg_if cg_for cg_index cgh_struct cgh_enum cgh_enumpl cg_enumcase cg_unwrap cg_match cg_enumctor cg_paymatch cg_matchdefault cg_matchguard cgh_enummethod cg_breakcont cgh_const cg_with cg_fnptr cg_closure cg_closurearg cg_matchpos cg_namedpl cg_print cg_mclosure cg_ifval cg_gpaymatch cg_builtins cg_lits cg_optchain cg_strco cg_interp cg_bmethods cg_genfunc cg_gentype cg_pmodes cg_throws cg_async cg_import cg_module cghelp cg_qualty cg_maperr cg_condhoist cg_matchhoist cg_docatch cg_gentail cg_amethod cg_channel cg_main cg_iflet cg_forproto cg_tpwild cg_paybindty cg_nestmatch cg_forpair cg_leaddot cg_valmatch cg_elseif cg_derive cg_derivespec cg_argsvec cg_opaque cg_box cg_moveonly cg_barecatch cg_hoistarms cg_tuplety cg_blockexpr cg_forelem cgh_global cg_orpat cg_withty cg_sinkarg cg_addrof cg_optflat cgh_requires cg_ifletval cgh_systypes cg_atomics cg_syncintr cg_sysstatic cg_linkattrs cgh_layout cgh_vectable cg_sysreg cg_rawmem cg_iteradapt cg_vecops cg_maperrclos cg_constgen cg_parallel cg_tupleparam cg_binprec cg_externabi cg_unitthrows)
    add_test(
        NAME    e2e_selfhost_codegen_hpp_${prog}
        COMMAND sh -c "mkdir -p ${cg_workdir}/hpp/${prog} && $<TARGET_FILE:vestra> build ${cg_dir}/${prog}.vst -o ${cg_workdir}/hpp/${prog} >/dev/null 2>&1; awk 'f{print} index(\$0, \"#endif  // VESTRA_RUNTIME_PRELUDE\"){f=1}' ${cg_workdir}/hpp/${prog}/${prog}.hpp > ${cg_workdir}/hpp/${prog}/${prog}_href.txt; ${e2e_workdir}/vgen --hpp ${cg_dir}/${prog}.vst > ${cg_workdir}/hpp/${prog}/${prog}_hgot.txt; diff ${cg_workdir}/hpp/${prog}/${prog}_href.txt ${cg_workdir}/hpp/${prog}/${prog}_hgot.txt")
    set_tests_properties(e2e_selfhost_codegen_hpp_${prog} PROPERTIES DEPENDS e2e_selfhost_codegen_compile)
endforeach()

# Shapes: exercises structs + member access + bare enum + match.
set(e2e_shapes_input  ${CMAKE_SOURCE_DIR}/examples/shapes.vst)
set(e2e_shapes_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_shapes.cpp)

add_test(
    NAME    e2e_shapes_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_shapes_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_shapes_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_shapes_driver} ${e2e_workdir}/shapes.cpp
            -o ${e2e_workdir}/shapes_app)
set_tests_properties(e2e_shapes_compile PROPERTIES DEPENDS e2e_shapes_transpile)

add_test(
    NAME    e2e_shapes_run
    COMMAND ${e2e_workdir}/shapes_app)
set_tests_properties(e2e_shapes_run PROPERTIES
    DEPENDS                e2e_shapes_compile
    PASS_REGULAR_EXPRESSION "compute\\(\\) = 44")

# Ownership: a sink parameter takes ownership of a binding; the caller can
# pre-borrow it (read) before transferring ownership at the end.
set(e2e_ownership_input  ${CMAKE_SOURCE_DIR}/examples/ownership.vst)
set(e2e_ownership_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_ownership.cpp)

add_test(
    NAME    e2e_ownership_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_ownership_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_ownership_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_ownership_driver} ${e2e_workdir}/ownership.cpp
            -o ${e2e_workdir}/ownership_app)
set_tests_properties(e2e_ownership_compile PROPERTIES DEPENDS e2e_ownership_transpile)

add_test(
    NAME    e2e_ownership_run
    COMMAND ${e2e_workdir}/ownership_app)
set_tests_properties(e2e_ownership_run PROPERTIES
    DEPENDS                e2e_ownership_compile
    PASS_REGULAR_EXPRESSION "compute\\(\\) = 21")

# Generics: exercises template instantiation in the host C++ compiler.
set(e2e_generics_input  ${CMAKE_SOURCE_DIR}/examples/generics.vst)
set(e2e_generics_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_generics.cpp)

add_test(
    NAME    e2e_generics_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_generics_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_generics_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_generics_driver} ${e2e_workdir}/generics.cpp
            -o ${e2e_workdir}/generics_app)
set_tests_properties(e2e_generics_compile PROPERTIES DEPENDS e2e_generics_transpile)

add_test(
    NAME    e2e_generics_run
    COMMAND ${e2e_workdir}/generics_app)
set_tests_properties(e2e_generics_run PROPERTIES
    DEPENDS                e2e_generics_compile
    PASS_REGULAR_EXPRESSION "compute\\(\\) = 42")

# Comptime: each `const` initializer folds to a literal in the emitted C++.
set(e2e_comptime_input  ${CMAKE_SOURCE_DIR}/examples/comptime.vst)
set(e2e_comptime_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_comptime.cpp)

add_test(
    NAME    e2e_comptime_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_comptime_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_comptime_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_comptime_driver} ${e2e_workdir}/comptime.cpp
            -o ${e2e_workdir}/comptime_app)
set_tests_properties(e2e_comptime_compile PROPERTIES DEPENDS e2e_comptime_transpile)

add_test(
    NAME    e2e_comptime_run
    COMMAND ${e2e_workdir}/comptime_app)
set_tests_properties(e2e_comptime_run PROPERTIES
    DEPENDS                e2e_comptime_compile
    PASS_REGULAR_EXPRESSION "compute\\(\\) = 6926")

# Cfg / @when: same source compiles to a different program per host. The
# regex only verifies "something numeric came out" — the per-host arms are
# checked by the unit tests in tests/sema/comptime_test.cpp.
set(e2e_cfg_input  ${CMAKE_SOURCE_DIR}/examples/cfg.vst)
set(e2e_cfg_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_cfg.cpp)

add_test(
    NAME    e2e_cfg_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_cfg_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_cfg_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_cfg_driver} ${e2e_workdir}/cfg.cpp
            -o ${e2e_workdir}/cfg_app)
set_tests_properties(e2e_cfg_compile PROPERTIES DEPENDS e2e_cfg_transpile)

add_test(
    NAME    e2e_cfg_run
    COMMAND ${e2e_workdir}/cfg_app)
set_tests_properties(e2e_cfg_run PROPERTIES
    DEPENDS                e2e_cfg_compile
    PASS_REGULAR_EXPRESSION "compute\\(\\) = [0-9]+")

# Vectors: precomputed table built at fold time, read at runtime.
set(e2e_vectors_input  ${CMAKE_SOURCE_DIR}/examples/vectors.vst)
set(e2e_vectors_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_vectors.cpp)

add_test(
    NAME    e2e_vectors_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_vectors_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_vectors_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_vectors_driver} ${e2e_workdir}/vectors.cpp
            -o ${e2e_workdir}/vectors_app)
set_tests_properties(e2e_vectors_compile PROPERTIES DEPENDS e2e_vectors_transpile)

add_test(
    NAME    e2e_vectors_run
    COMMAND ${e2e_workdir}/vectors_app)
set_tests_properties(e2e_vectors_run PROPERTIES
    DEPENDS                e2e_vectors_compile
    PASS_REGULAR_EXPRESSION "compute\\(\\) = 36")

# Sin table: §12.1 comptime math stdlib + vectors. A 32-sample sine
# cycle is built entirely at fold time (sin/tau dispatched to the
# builtin layer), baked into the emitted C++ as a constexpr
# std::array<double, 32>, then sum-checked at runtime against a
# tight epsilon.
set(e2e_sin_table_input  ${CMAKE_SOURCE_DIR}/examples/sin_table.vst)
set(e2e_sin_table_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_sin_table.cpp)

add_test(
    NAME    e2e_sin_table_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_sin_table_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_sin_table_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_sin_table_driver} ${e2e_workdir}/sin_table.cpp
            -o ${e2e_workdir}/sin_table_app)
set_tests_properties(e2e_sin_table_compile PROPERTIES DEPENDS e2e_sin_table_transpile)

add_test(
    NAME    e2e_sin_table_run
    COMMAND ${e2e_workdir}/sin_table_app)
set_tests_properties(e2e_sin_table_run PROPERTIES
    DEPENDS                e2e_sin_table_compile
    PASS_REGULAR_EXPRESSION "sin_table OK")

# Embed: §12.1 @embed reads embed_data.txt at compile time and bakes its
# 14 bytes into a constexpr std::array<std::uint8_t, 14> in the emitted
# header. The runtime side sums them (= 1128) and reads byte 0/13.
set(e2e_embed_input  ${CMAKE_SOURCE_DIR}/examples/embed.vst)
set(e2e_embed_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_embed.cpp)

add_test(
    NAME    e2e_embed_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_embed_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_embed_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_embed_driver} ${e2e_workdir}/embed.cpp
            -o ${e2e_workdir}/embed_app)
set_tests_properties(e2e_embed_compile PROPERTIES DEPENDS e2e_embed_transpile)

add_test(
    NAME    e2e_embed_run
    COMMAND ${e2e_workdir}/embed_app)
set_tests_properties(e2e_embed_run PROPERTIES
    DEPENDS                e2e_embed_compile
    PASS_REGULAR_EXPRESSION "checksum\\(\\) = 1128")

# Interpolation: §4 `"text \(expr) text"` lowers to std::format. The
# runtime compares the rendered strings byte-for-byte against expected
# values and prints "interpolation OK" only on full match.
set(e2e_interp_input  ${CMAKE_SOURCE_DIR}/examples/interpolation.vst)
set(e2e_interp_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_interpolation.cpp)

add_test(
    NAME    e2e_interpolation_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_interp_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_interpolation_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_interp_driver} ${e2e_workdir}/interpolation.cpp
            -o ${e2e_workdir}/interpolation_app)
set_tests_properties(e2e_interpolation_compile PROPERTIES DEPENDS e2e_interpolation_transpile)

add_test(
    NAME    e2e_interpolation_run
    COMMAND ${e2e_workdir}/interpolation_app)
set_tests_properties(e2e_interpolation_run PROPERTIES
    DEPENDS                e2e_interpolation_compile
    PASS_REGULAR_EXPRESSION "interpolation OK")

# Reflection: §12.2 `StructName.fields` folds to a [N]Str vector of
# field names. The runtime side reads name_at(i) for each i and
# byte-compares; pass if all three names round-trip.
set(e2e_refl_input  ${CMAKE_SOURCE_DIR}/examples/reflection.vst)
set(e2e_refl_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_reflection.cpp)

add_test(
    NAME    e2e_reflection_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_refl_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_reflection_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_refl_driver} ${e2e_workdir}/reflection.cpp
            -o ${e2e_workdir}/reflection_app)
set_tests_properties(e2e_reflection_compile PROPERTIES DEPENDS e2e_reflection_transpile)

add_test(
    NAME    e2e_reflection_run
    COMMAND ${e2e_workdir}/reflection_app)
set_tests_properties(e2e_reflection_run PROPERTIES
    DEPENDS                e2e_reflection_compile
    PASS_REGULAR_EXPRESSION "reflection OK")

# Match: payloaded enum + std::visit-based match expression lowering.
# Runtime side calls area() on each Shape variant individually and on
# a sum-of-three; pass if both come out to the expected values.
set(e2e_match_input  ${CMAKE_SOURCE_DIR}/examples/match.vst)
set(e2e_match_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_match.cpp)

add_test(
    NAME    e2e_match_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_match_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_match_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_match_driver} ${e2e_workdir}/match.cpp
            -o ${e2e_workdir}/match_app)
set_tests_properties(e2e_match_compile PROPERTIES DEPENDS e2e_match_transpile)

add_test(
    NAME    e2e_match_run
    COMMAND ${e2e_workdir}/match_app)
set_tests_properties(e2e_match_run PROPERTIES
    DEPENDS                e2e_match_compile
    PASS_REGULAR_EXPRESSION "match OK")

# derive(Eq): structural equality lowered to a defaulted operator==
# for both a struct and a payloaded enum. Runtime side compares
# matching / mismatching values and asserts the result.
set(e2e_derive_input  ${CMAKE_SOURCE_DIR}/examples/derive_eq.vst)
set(e2e_derive_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_derive_eq.cpp)

add_test(
    NAME    e2e_derive_eq_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_derive_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_derive_eq_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_derive_driver} ${e2e_workdir}/derive_eq.cpp
            -o ${e2e_workdir}/derive_eq_app)
set_tests_properties(e2e_derive_eq_compile PROPERTIES DEPENDS e2e_derive_eq_transpile)

add_test(
    NAME    e2e_derive_eq_run
    COMMAND ${e2e_workdir}/derive_eq_app)
set_tests_properties(e2e_derive_eq_run PROPERTIES
    DEPENDS                e2e_derive_eq_compile
    PASS_REGULAR_EXPRESSION "derive\\(Eq\\) OK")

# derive(Hash): std::hash<T> specialization (with derive(Eq)) → Point
# is usable as a std::unordered_map key. Runtime side bucket-places,
# overwrites a key, looks up, and sanity-checks the hash distribution.
set(e2e_hash_input  ${CMAKE_SOURCE_DIR}/examples/derive_hash.vst)
set(e2e_hash_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_derive_hash.cpp)

add_test(
    NAME    e2e_derive_hash_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_hash_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_derive_hash_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_hash_driver} ${e2e_workdir}/derive_hash.cpp
            -o ${e2e_workdir}/derive_hash_app)
set_tests_properties(e2e_derive_hash_compile PROPERTIES DEPENDS e2e_derive_hash_transpile)

add_test(
    NAME    e2e_derive_hash_run
    COMMAND ${e2e_workdir}/derive_hash_app)
set_tests_properties(e2e_derive_hash_run PROPERTIES
    DEPENDS                e2e_derive_hash_compile
    PASS_REGULAR_EXPRESSION "derive\\(Hash\\) OK")

# Optional: §9 `T?` end-to-end — `nil` / `??` / `if let` / `!` plus the
# implicit T → T? wrap, all lowering to std::optional<T>. The runtime
# side exercises every branch (present + absent, force-unwrap success +
# the bad_optional_access panic) and prints "optional OK" only on full
# pass.
set(e2e_opt_input  ${CMAKE_SOURCE_DIR}/examples/optional.vst)
set(e2e_opt_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_optional.cpp)

add_test(
    NAME    e2e_optional_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_opt_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_optional_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_opt_driver} ${e2e_workdir}/optional.cpp
            -o ${e2e_workdir}/optional_app)
set_tests_properties(e2e_optional_compile PROPERTIES DEPENDS e2e_optional_transpile)

add_test(
    NAME    e2e_optional_run
    COMMAND ${e2e_workdir}/optional_app)
set_tests_properties(e2e_optional_run PROPERTIES
    DEPENDS                e2e_optional_compile
    PASS_REGULAR_EXPRESSION "optional OK")

# Tuples: §6 tuple types, literals, and destructuring patterns. Types
# lower to std::tuple<...>; literals to std::tuple{...} (CTAD);
# destructuring `let (a, b) = ...` to structured bindings. E2E
# returns multiple values from a div_mod, destructures with both
# bindings, the wildcard form, and a nested tuple pattern.
set(e2e_tuples_input  ${CMAKE_SOURCE_DIR}/examples/tuples.vst)
set(e2e_tuples_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_tuples.cpp)

add_test(
    NAME    e2e_tuples_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_tuples_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_tuples_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_tuples_driver} ${e2e_workdir}/tuples.cpp
            -o ${e2e_workdir}/tuples_app)
set_tests_properties(e2e_tuples_compile PROPERTIES DEPENDS e2e_tuples_transpile)

add_test(
    NAME    e2e_tuples_run
    COMMAND ${e2e_workdir}/tuples_app)
set_tests_properties(e2e_tuples_run PROPERTIES
    DEPENDS                e2e_tuples_compile
    PASS_REGULAR_EXPRESSION "tuples OK")

# Nested tuples: §6 nested tuple destructuring. C++ structured bindings
# are single-level; the codegen hoists each nested sub-tuple into a
# sibling `auto [...] = placeholder;` follow-on statement so arbitrarily
# deep tuple patterns round-trip. E2E exercises two-level let/var,
# wildcard at an inner slot, and three-level nesting.
set(e2e_nested_tuples_input  ${CMAKE_SOURCE_DIR}/examples/nested_tuples.vst)
set(e2e_nested_tuples_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_nested_tuples.cpp)

add_test(
    NAME    e2e_nested_tuples_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_nested_tuples_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_nested_tuples_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_nested_tuples_driver} ${e2e_workdir}/nested_tuples.cpp
            -o ${e2e_workdir}/nested_tuples_app)
set_tests_properties(e2e_nested_tuples_compile PROPERTIES DEPENDS e2e_nested_tuples_transpile)

add_test(
    NAME    e2e_nested_tuples_run
    COMMAND ${e2e_workdir}/nested_tuples_app)
set_tests_properties(e2e_nested_tuples_run PROPERTIES
    DEPENDS                e2e_nested_tuples_compile
    PASS_REGULAR_EXPRESSION "nested tuples OK")

# Tuple params + match-arm tuple patterns: §6 tuple patterns at sites
# other than `let` / `var` / `for`. Param slot: codegen emits a
# synthetic `__vstr_arg{i}` plus a structured-binding prologue at the
# top of the body. Match-arm: `case .point((let x, let y)):` destructures
# the payload field inline.
set(e2e_tuple_params_input  ${CMAKE_SOURCE_DIR}/examples/tuple_params.vst)
set(e2e_tuple_params_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_tuple_params.cpp)

add_test(
    NAME    e2e_tuple_params_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_tuple_params_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_tuple_params_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_tuple_params_driver} ${e2e_workdir}/tuple_params.cpp
            -o ${e2e_workdir}/tuple_params_app)
set_tests_properties(e2e_tuple_params_compile PROPERTIES DEPENDS e2e_tuple_params_transpile)

add_test(
    NAME    e2e_tuple_params_run
    COMMAND ${e2e_workdir}/tuple_params_app)
set_tests_properties(e2e_tuple_params_run PROPERTIES
    DEPENDS                e2e_tuple_params_compile
    PASS_REGULAR_EXPRESSION "tuple params OK")

# Mid-expression try: §9 IfExpr branches that contain a propagating
# `try`. The codegen hoists the whole conditional into a lambda
# returning std::expected<T, E>; the outer scope unwraps via the
# canonical 3-line escape so propagation actually escapes the enclosing
# throws(E) function. Used to fall back to .value() (panic) under v0.5.
set(e2e_midtry_input  ${CMAKE_SOURCE_DIR}/examples/midtry.vst)
set(e2e_midtry_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_midtry.cpp)

add_test(
    NAME    e2e_midtry_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_midtry_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_midtry_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_midtry_driver} ${e2e_workdir}/midtry.cpp
            -o ${e2e_workdir}/midtry_app)
set_tests_properties(e2e_midtry_compile PROPERTIES DEPENDS e2e_midtry_transpile)

add_test(
    NAME    e2e_midtry_run
    COMMAND ${e2e_workdir}/midtry_app)
set_tests_properties(e2e_midtry_run PROPERTIES
    DEPENDS                e2e_midtry_compile
    PASS_REGULAR_EXPRESSION "midtry OK")

# Mid-expression try inside match arms: §9 cond-hoist for MatchExpr.
# Sibling of the IfExpr cond-hoist shipped in 84f718e: a MatchExpr whose
# arm bodies contain a propagating try lifts to a lambda returning
# std::expected<T, E>. Bare enum lowers to a statement-form switch;
# payloaded enum to an if-chain over std::holds_alternative (std::visit
# can't be reused because its lambda swallows the propagation).
set(e2e_midmatch_input  ${CMAKE_SOURCE_DIR}/examples/midmatch.vst)
set(e2e_midmatch_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_midmatch.cpp)

add_test(
    NAME    e2e_midmatch_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_midmatch_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_midmatch_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_midmatch_driver} ${e2e_workdir}/midmatch.cpp
            -o ${e2e_workdir}/midmatch_app)
set_tests_properties(e2e_midmatch_compile PROPERTIES DEPENDS e2e_midmatch_transpile)

add_test(
    NAME    e2e_midmatch_run
    COMMAND ${e2e_workdir}/midmatch_app)
set_tests_properties(e2e_midmatch_run PROPERTIES
    DEPENDS                e2e_midmatch_compile
    PASS_REGULAR_EXPRESSION "midmatch OK")

# derive(Default): §12.3 Default protocol — completes the derive set
# (Eq / Hash / Clone / Debug / Display). `T.default()` lowers to value-
# init `T{}` when every field of T is Default-conformant (primitives,
# Optional, nested struct that also derives Default). Sema rejects a
# derive on a struct with a non-conformant field.
set(e2e_derive_default_input  ${CMAKE_SOURCE_DIR}/examples/derive_default.vst)
set(e2e_derive_default_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_derive_default.cpp)

add_test(
    NAME    e2e_derive_default_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_derive_default_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_derive_default_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_derive_default_driver} ${e2e_workdir}/derive_default.cpp
            -o ${e2e_workdir}/derive_default_app)
set_tests_properties(e2e_derive_default_compile PROPERTIES DEPENDS e2e_derive_default_transpile)

add_test(
    NAME    e2e_derive_default_run
    COMMAND ${e2e_workdir}/derive_default_app)
set_tests_properties(e2e_derive_default_run PROPERTIES
    DEPENDS                e2e_derive_default_compile
    PASS_REGULAR_EXPRESSION "derive\\(Default\\) OK")

# Panic primitives: §10 `panic(msg)` / `abort()` / `unreachable()`.
# Each returns `Never` (bottom type) so the call fits any expression
# slot through `__vstr::Never`'s templated [[noreturn]] conversion op.
# Force-unwrap (`!`) and `try!` now route through the same named
# primitive instead of std::bad_optional_access / std::bad_expected_access
# exceptions. The driver fork+aborts the panic paths so the parent
# can verify the abort happened without itself dying.
set(e2e_panic_input  ${CMAKE_SOURCE_DIR}/examples/panic.vst)
set(e2e_panic_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_panic.cpp)

add_test(
    NAME    e2e_panic_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_panic_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_panic_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_panic_driver} ${e2e_workdir}/panic.cpp
            -o ${e2e_workdir}/panic_app)
set_tests_properties(e2e_panic_compile PROPERTIES DEPENDS e2e_panic_transpile)

add_test(
    NAME    e2e_panic_run
    COMMAND ${e2e_workdir}/panic_app)
set_tests_properties(e2e_panic_run PROPERTIES
    DEPENDS                e2e_panic_compile
    PASS_REGULAR_EXPRESSION "panic OK")

# Span[T] / MutSpan[T]: §10 borrowed, non-escapable view types.
# Span[T] → std::span<const T>, MutSpan[T] → std::span<T>. A fixed
# array `[N]T` slides into either at a call site without an explicit
# cast (TypeArena::assignable accepts the coercion; std::span's
# std::array-constructible ctor handles the C++ side). `.count` is
# Int via static_cast<std::intptr_t>(s.size()); `.isEmpty` is .empty();
# indexing wraps the index in static_cast<std::size_t> for the
# std::span operator[] convention.
set(e2e_spans_input  ${CMAKE_SOURCE_DIR}/examples/spans.vst)
set(e2e_spans_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_spans.cpp)

add_test(
    NAME    e2e_spans_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_spans_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_spans_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_spans_driver} ${e2e_workdir}/spans.cpp
            -o ${e2e_workdir}/spans_app)
set_tests_properties(e2e_spans_compile PROPERTIES DEPENDS e2e_spans_transpile)

add_test(
    NAME    e2e_spans_run
    COMMAND ${e2e_workdir}/spans_app)
set_tests_properties(e2e_spans_run PROPERTIES
    DEPENDS                e2e_spans_compile
    PASS_REGULAR_EXPRESSION "spans OK")

# Optional in Display: §4 interpolation now accepts an Optional<T>
# splice when T is itself Display-conformant. The runtime preamble
# carries a std::formatter<std::optional<T>> partial specialization
# (constrained on std::formattable<T, char>) that renders `.none` as
# "nil" and delegates `.some(v)` to T's formatter — composes with
# derive(Display) / derive(Debug) on user types.
set(e2e_optdisp_input  ${CMAKE_SOURCE_DIR}/examples/optional_display.vst)
set(e2e_optdisp_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_optional_display.cpp)

add_test(
    NAME    e2e_optional_display_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_optdisp_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_optional_display_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_optdisp_driver} ${e2e_workdir}/optional_display.cpp
            -o ${e2e_workdir}/optional_display_app)
set_tests_properties(e2e_optional_display_compile PROPERTIES DEPENDS e2e_optional_display_transpile)

add_test(
    NAME    e2e_optional_display_run
    COMMAND ${e2e_workdir}/optional_display_app)
set_tests_properties(e2e_optional_display_run PROPERTIES
    DEPENDS                e2e_optional_display_compile
    PASS_REGULAR_EXPRESSION "optional display OK")

# Match-pattern enhancements: §17.7 brings literal / or- / range
# patterns and tuple-scrutinee match into the basic match story.
# Bare-enum or-patterns stack as switch labels; non-enum scrutinees
# go through an if-else-if chain with per-pattern predicates;
# tuple patterns compose via std::get<i> on the bound scrutinee.
set(e2e_match_patterns_input  ${CMAKE_SOURCE_DIR}/examples/match_patterns.vst)
set(e2e_match_patterns_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_match_patterns.cpp)

add_test(
    NAME    e2e_match_patterns_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_match_patterns_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_match_patterns_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_match_patterns_driver} ${e2e_workdir}/match_patterns.cpp
            -o ${e2e_workdir}/match_patterns_app)
set_tests_properties(e2e_match_patterns_compile PROPERTIES DEPENDS e2e_match_patterns_transpile)

add_test(
    NAME    e2e_match_patterns_run
    COMMAND ${e2e_workdir}/match_patterns_app)
set_tests_properties(e2e_match_patterns_run PROPERTIES
    DEPENDS                e2e_match_patterns_compile
    PASS_REGULAR_EXPRESSION "match patterns OK")

# Opaque-newtype follow-ons: §3 derive(Hash, Debug, Display) for an
# `opaque type Q = T` newtype. The codegen emits std::hash<Q> +
# std::formatter<Q> specs that delegate to the underlying T via
# static_cast<T>(q). The e2e also exercises the inverse conversion
# `T(q)` (Vestra-side symmetric counterpart of `Q(t)`).
set(e2e_newtype_input  ${CMAKE_SOURCE_DIR}/examples/newtype.vst)
set(e2e_newtype_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_newtype.cpp)

add_test(
    NAME    e2e_newtype_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_newtype_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_newtype_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_newtype_driver} ${e2e_workdir}/newtype.cpp
            -o ${e2e_workdir}/newtype_app)
set_tests_properties(e2e_newtype_compile PROPERTIES DEPENDS e2e_newtype_transpile)

add_test(
    NAME    e2e_newtype_run
    COMMAND ${e2e_workdir}/newtype_app)
set_tests_properties(e2e_newtype_run PROPERTIES
    DEPENDS                e2e_newtype_compile
    PASS_REGULAR_EXPRESSION "opaque newtype OK")

# Map-error: §9 `.mapError(_ f)` on Result — widens the error type
# between throws functions by running a closure on the error path.
# Codegen lowers to std::expected::transform_error, so the runtime
# cost is one indirect call on error and zero on success.
set(e2e_map_error_input  ${CMAKE_SOURCE_DIR}/examples/map_error.vst)
set(e2e_map_error_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_map_error.cpp)

add_test(
    NAME    e2e_map_error_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_map_error_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_map_error_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_map_error_driver} ${e2e_workdir}/map_error.cpp
            -o ${e2e_workdir}/map_error_app)
set_tests_properties(e2e_map_error_compile PROPERTIES DEPENDS e2e_map_error_transpile)

add_test(
    NAME    e2e_map_error_run
    COMMAND ${e2e_workdir}/map_error_app)
set_tests_properties(e2e_map_error_run PROPERTIES
    DEPENDS                e2e_map_error_compile
    PASS_REGULAR_EXPRESSION "mapError OK")

# With-binding: §17.4 `with NAME = EXPR { ... }` value-binding form
# distinct from the existing cap-typed form (`with Alloc { ... }`).
# Lowers to a C++ sub-scope with `auto&& NAME = EXPR;`. Parser
# disambiguates by the leading identifier's first letter (lowercase
# = name binding, uppercase = capability type) per Vestra convention.
set(e2e_with_binding_input  ${CMAKE_SOURCE_DIR}/examples/with_binding.vst)
set(e2e_with_binding_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_with_binding.cpp)

add_test(
    NAME    e2e_with_binding_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_with_binding_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_with_binding_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_with_binding_driver} ${e2e_workdir}/with_binding.cpp
            -o ${e2e_workdir}/with_binding_app)
set_tests_properties(e2e_with_binding_compile PROPERTIES DEPENDS e2e_with_binding_transpile)

add_test(
    NAME    e2e_with_binding_run
    COMMAND ${e2e_workdir}/with_binding_app)
set_tests_properties(e2e_with_binding_run PROPERTIES
    DEPENDS                e2e_with_binding_compile
    PASS_REGULAR_EXPRESSION "with binding OK")

# Do/catch where guard: §9 `catch (e: E) where guard { … }`. The
# guard runs with the bound error in scope; on true the handler
# applies, on false v0.5 panics via the named primitive (the e2e
# driver verifies the panic path via fork+SIGABRT).
set(e2e_docatch_where_input  ${CMAKE_SOURCE_DIR}/examples/docatch_where.vst)
set(e2e_docatch_where_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_docatch_where.cpp)

add_test(
    NAME    e2e_docatch_where_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_docatch_where_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_docatch_where_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_docatch_where_driver} ${e2e_workdir}/docatch_where.cpp
            -o ${e2e_workdir}/docatch_where_app)
set_tests_properties(e2e_docatch_where_compile PROPERTIES DEPENDS e2e_docatch_where_transpile)

add_test(
    NAME    e2e_docatch_where_run
    COMMAND ${e2e_workdir}/docatch_where_app)
set_tests_properties(e2e_docatch_where_run PROPERTIES
    DEPENDS                e2e_docatch_where_compile
    PASS_REGULAR_EXPRESSION "docatch where OK")

# Box[T]: §10 unique-ownership heap pointer. Lowers to
# std::unique_ptr<T>; Box.new(v) → std::make_unique<T>(v); box.value
# → *box. Allocation requires the §8 `Alloc` capability. E2E pins
# both the unique-ptr lowering and move-only property via
# static_assert, then round-trips through Cell-nesting.
set(e2e_boxes_input  ${CMAKE_SOURCE_DIR}/examples/boxes.vst)
set(e2e_boxes_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_boxes.cpp)

add_test(
    NAME    e2e_boxes_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_boxes_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_boxes_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_boxes_driver} ${e2e_workdir}/boxes.cpp
            -o ${e2e_workdir}/boxes_app)
set_tests_properties(e2e_boxes_compile PROPERTIES DEPENDS e2e_boxes_transpile)

add_test(
    NAME    e2e_boxes_run
    COMMAND ${e2e_workdir}/boxes_app)
set_tests_properties(e2e_boxes_run PROPERTIES
    DEPENDS                e2e_boxes_compile
    PASS_REGULAR_EXPRESSION "boxes OK")

# Layout attributes: §4 @repr / @align / @bits. The packed wire
# header occupies exactly 6 bytes; the cache-line struct is
# 64-byte aligned; two 4-bit nibbles share a byte. The e2e uses
# static_assert to pin every layout invariant, then exercises the
# bit-field round-trip at runtime.
set(e2e_layout_input  ${CMAKE_SOURCE_DIR}/examples/layout.vst)
set(e2e_layout_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_layout.cpp)

add_test(
    NAME    e2e_layout_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_layout_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_layout_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_layout_driver} ${e2e_workdir}/layout.cpp
            -o ${e2e_workdir}/layout_app)
set_tests_properties(e2e_layout_compile PROPERTIES DEPENDS e2e_layout_transpile)

add_test(
    NAME    e2e_layout_run
    COMMAND ${e2e_workdir}/layout_app)
set_tests_properties(e2e_layout_run PROPERTIES
    DEPENDS                e2e_layout_compile
    PASS_REGULAR_EXPRESSION "layout OK")

# Opaque type: §3 zero-cost newtype. Each `opaque type Q = T` lowers
# to `enum class Q : T {}`; `Q(t)` is `static_cast<Q>(t)`; `q.value`
# is `static_cast<T>(q)`. The e2e round-trips a value through both
# directions and uses static_assert to pin the distinctness of two
# opaques over the same underlying.
set(e2e_opaque_input  ${CMAKE_SOURCE_DIR}/examples/opaque.vst)
set(e2e_opaque_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_opaque.cpp)

add_test(
    NAME    e2e_opaque_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_opaque_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_opaque_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_opaque_driver} ${e2e_workdir}/opaque.cpp
            -o ${e2e_workdir}/opaque_app)
set_tests_properties(e2e_opaque_compile PROPERTIES DEPENDS e2e_opaque_transpile)

add_test(
    NAME    e2e_opaque_run
    COMMAND ${e2e_workdir}/opaque_app)
set_tests_properties(e2e_opaque_run PROPERTIES
    DEPENDS                e2e_opaque_compile
    PASS_REGULAR_EXPRESSION "opaque OK")

# Display: §4 Display-conformance check on interpolation splices.
# Primitives ride free; user types need derive(Display) (or Debug).
# Both derives lower to a std::formatter spec the std::format call
# from the interpolation can pick up. Runtime renders a Point + Shape
# + Int splice into one string and byte-checks the result.
set(e2e_disp_input  ${CMAKE_SOURCE_DIR}/examples/display.vst)
set(e2e_disp_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_display.cpp)

add_test(
    NAME    e2e_display_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_disp_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_display_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_disp_driver} ${e2e_workdir}/display.cpp
            -o ${e2e_workdir}/display_app)
set_tests_properties(e2e_display_compile PROPERTIES DEPENDS e2e_display_transpile)

add_test(
    NAME    e2e_display_run
    COMMAND ${e2e_workdir}/display_app)
set_tests_properties(e2e_display_run PROPERTIES
    DEPENDS                e2e_display_compile
    PASS_REGULAR_EXPRESSION "display OK")

# &decl + @extern("conv") (§A12, §14.6.1 + §14.6.3). Address-of a
# static yields Ptr[T]; the codegen emits a plain `&` so the
# symbol's address is meaningful at link time. @extern accepts
# any of the named convs (C, hvc64, smc64, ..., interrupt); v0.5
# hosted validates the conv string but emits no extra attribute.
set(e2e_addr_of_input  ${CMAKE_SOURCE_DIR}/examples/addr_of.vst)
set(e2e_addr_of_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_addr_of.cpp)

add_test(
    NAME    e2e_addr_of_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_addr_of_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_addr_of_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_addr_of_driver} ${e2e_workdir}/addr_of.cpp
            -o ${e2e_workdir}/addr_of_app)
set_tests_properties(e2e_addr_of_compile PROPERTIES
    DEPENDS e2e_addr_of_transpile)

add_test(
    NAME    e2e_addr_of_run
    COMMAND ${e2e_workdir}/addr_of_app)
set_tests_properties(e2e_addr_of_run PROPERTIES
    DEPENDS                e2e_addr_of_compile
    PASS_REGULAR_EXPRESSION "addr_of OK")

# PerCpu[T] (§A11, §14.8). First slice — type-system surface +
# .mine() method synth. Single-slot 64-byte-aligned storage on
# hosted; the kernel target swaps to [MAX_HARTS]Padded<T> indexed
# by a hart register. The runtime drives PerCpu[Atomic[UInt32]]
# through fetchAdd / load and confirms the no-false-sharing
# alignment statically.
set(e2e_per_cpu_input  ${CMAKE_SOURCE_DIR}/examples/per_cpu.vst)
set(e2e_per_cpu_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_per_cpu.cpp)

add_test(
    NAME    e2e_per_cpu_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_per_cpu_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_per_cpu_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_per_cpu_driver} ${e2e_workdir}/per_cpu.cpp
            -o ${e2e_workdir}/per_cpu_app)
set_tests_properties(e2e_per_cpu_compile PROPERTIES
    DEPENDS e2e_per_cpu_transpile)

add_test(
    NAME    e2e_per_cpu_run
    COMMAND ${e2e_workdir}/per_cpu_app)
set_tests_properties(e2e_per_cpu_run PROPERTIES
    DEPENDS                e2e_per_cpu_compile
    PASS_REGULAR_EXPRESSION "per_cpu OK")

# InterruptsOff scope + Scheduler.swapContext (§A7, §14.13 + §14.14).
# `with InterruptsOff { ... }` is a brace-block on hosted (the
# capability checker statically enforces the call-shape rules);
# Context is an opaque __vstr struct with fixed bytes;
# Scheduler.swapContext lowers to a __vstr no-op shim. The runtime
# drives critical_bump, nested_bump, switch_tasks plus static_asserts
# on Context's size/alignment.
set(e2e_interrupts_off_input  ${CMAKE_SOURCE_DIR}/examples/interrupts_off.vst)
set(e2e_interrupts_off_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_interrupts_off.cpp)

add_test(
    NAME    e2e_interrupts_off_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_interrupts_off_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_interrupts_off_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_interrupts_off_driver} ${e2e_workdir}/interrupts_off.cpp
            -o ${e2e_workdir}/interrupts_off_app)
set_tests_properties(e2e_interrupts_off_compile PROPERTIES
    DEPENDS e2e_interrupts_off_transpile)

add_test(
    NAME    e2e_interrupts_off_run
    COMMAND ${e2e_workdir}/interrupts_off_app)
set_tests_properties(e2e_interrupts_off_run PROPERTIES
    DEPENDS                e2e_interrupts_off_compile
    PASS_REGULAR_EXPRESSION "interrupts_off OK")

# @panic_handler (§A10, §15.5). First slice — sema validates the
# (Str, StrConst, Int) -> Never signature; codegen emits the user's
# handler with [[gnu::used]] plus a static-init block that writes
# its address into __vstr::panic_handler before main runs. The
# runtime panic shim consults the slot and delegates when non-null.
# The e2e exercises the registration (slot non-null) and the
# delegation (fork + SIGABRT confirms the handler reached abort).
set(e2e_panic_handler_input  ${CMAKE_SOURCE_DIR}/examples/panic_handler.vst)
set(e2e_panic_handler_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_panic_handler.cpp)

add_test(
    NAME    e2e_panic_handler_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_panic_handler_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_panic_handler_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_panic_handler_driver} ${e2e_workdir}/panic_handler.cpp
            -o ${e2e_workdir}/panic_handler_app)
set_tests_properties(e2e_panic_handler_compile PROPERTIES
    DEPENDS e2e_panic_handler_transpile)

add_test(
    NAME    e2e_panic_handler_run
    COMMAND ${e2e_workdir}/panic_handler_app)
set_tests_properties(e2e_panic_handler_run PROPERTIES
    DEPENDS                e2e_panic_handler_compile
    PASS_REGULAR_EXPRESSION "panic_handler OK")

# @interrupt handlers (§A8, §14.5.2). First slice — the func shape
# (inout TrapFrame, Unit return, no Alloc/Async/Extern) plus
# [[gnu::used]] codegen so the symbol survives DCE. The full
# §14.5.3 vector-table lowering waits on a follow-up. The runtime
# invokes the handlers directly with a stack TrapFrame and checks
# the inout-side mutations.
set(e2e_interrupt_handler_input  ${CMAKE_SOURCE_DIR}/examples/interrupt_handler.vst)
set(e2e_interrupt_handler_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_interrupt_handler.cpp)

add_test(
    NAME    e2e_interrupt_handler_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_interrupt_handler_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_interrupt_handler_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_interrupt_handler_driver} ${e2e_workdir}/interrupt_handler.cpp
            -o ${e2e_workdir}/interrupt_handler_app)
set_tests_properties(e2e_interrupt_handler_compile PROPERTIES
    DEPENDS e2e_interrupt_handler_transpile)

add_test(
    NAME    e2e_interrupt_handler_run
    COMMAND ${e2e_workdir}/interrupt_handler_app)
set_tests_properties(e2e_interrupt_handler_run PROPERTIES
    DEPENDS                e2e_interrupt_handler_compile
    PASS_REGULAR_EXPRESSION "interrupt_handler OK")

# MMIO views (§A6, §14.11). MmioView[T] / MmioRegion[T] over
# primitive T; constructors gated on Mmio (with the MutPtr coming
# from §A3's RawMemory mints). Codegen wraps a volatile T* so every
# read/write touches the bus. The runtime drives a single-register
# UInt32 + a 4-slot region.
set(e2e_mmio_input  ${CMAKE_SOURCE_DIR}/examples/mmio.vst)
set(e2e_mmio_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_mmio.cpp)

add_test(
    NAME    e2e_mmio_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_mmio_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_mmio_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_mmio_driver} ${e2e_workdir}/mmio.cpp
            -o ${e2e_workdir}/mmio_app)
set_tests_properties(e2e_mmio_compile PROPERTIES
    DEPENDS e2e_mmio_transpile)

add_test(
    NAME    e2e_mmio_run
    COMMAND ${e2e_workdir}/mmio_app)
set_tests_properties(e2e_mmio_run PROPERTIES
    DEPENDS                e2e_mmio_compile
    PASS_REGULAR_EXPRESSION "mmio OK")

# MmioWireView (§A6 follow-up, §14.11.3). Endianness-aware MMIO
# view: the constructor pins the bus endianness; reads/writes
# byteswap when the device endianness differs from host-native
# via std::byteswap. The runtime exercises a big-endian wire on
# a UInt32 cell (the raw bytes after a write reveal the swap on
# LE hosts) and a native pin (verbatim).
set(e2e_mmio_wire_input  ${CMAKE_SOURCE_DIR}/examples/mmio_wire.vst)
set(e2e_mmio_wire_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_mmio_wire.cpp)

add_test(
    NAME    e2e_mmio_wire_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_mmio_wire_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_mmio_wire_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_mmio_wire_driver} ${e2e_workdir}/mmio_wire.cpp
            -o ${e2e_workdir}/mmio_wire_app)
set_tests_properties(e2e_mmio_wire_compile PROPERTIES
    DEPENDS e2e_mmio_wire_transpile)

add_test(
    NAME    e2e_mmio_wire_run
    COMMAND ${e2e_workdir}/mmio_wire_app)
set_tests_properties(e2e_mmio_wire_run PROPERTIES
    DEPENDS                e2e_mmio_wire_compile
    PASS_REGULAR_EXPRESSION "mmio_wire OK")

# @repr(union) for bit-field register decoding (§A6 follow-up,
# §14.11.4). Two structs compose: a @repr(packed) sub-struct
# holding @bits(N) fields (the bit view) and a @repr(union)
# outer struct overlaying the raw word and the bit view. The
# runtime probes the bit ordering with seven crafted words and
# verifies each field's decode matches the expected mask.
set(e2e_mmio_union_input  ${CMAKE_SOURCE_DIR}/examples/mmio_union.vst)
set(e2e_mmio_union_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_mmio_union.cpp)

add_test(
    NAME    e2e_mmio_union_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_mmio_union_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_mmio_union_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_mmio_union_driver} ${e2e_workdir}/mmio_union.cpp
            -o ${e2e_workdir}/mmio_union_app)
set_tests_properties(e2e_mmio_union_compile PROPERTIES
    DEPENDS e2e_mmio_union_transpile)

add_test(
    NAME    e2e_mmio_union_run
    COMMAND ${e2e_workdir}/mmio_union_app)
set_tests_properties(e2e_mmio_union_run PROPERTIES
    DEPENDS                e2e_mmio_union_compile
    PASS_REGULAR_EXPRESSION "mmio_union OK")

# §A12 follow-up — function-pointer type + `&func` (§14.6.3).
# Vestra function-pointer spelling `(T1, ...) -> R` lowers to
# `R(*name)(const T1&, ...)`; `&func` produces the function-
# pointer value. The runtime exercises the dispatch-table
# pattern via let-bindings (`let a: (Int32) -> Int32 = &handler_a`)
# and the by-parameter pattern (`apply(handler_b, ...)`).
set(e2e_fn_ptr_input  ${CMAKE_SOURCE_DIR}/examples/fn_ptr_table.vst)
set(e2e_fn_ptr_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_fn_ptr_table.cpp)

add_test(
    NAME    e2e_fn_ptr_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_fn_ptr_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_fn_ptr_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_fn_ptr_driver} ${e2e_workdir}/fn_ptr_table.cpp
            -o ${e2e_workdir}/fn_ptr_table_app)
set_tests_properties(e2e_fn_ptr_compile PROPERTIES
    DEPENDS e2e_fn_ptr_transpile)

add_test(
    NAME    e2e_fn_ptr_run
    COMMAND ${e2e_workdir}/fn_ptr_table_app)
set_tests_properties(e2e_fn_ptr_run PROPERTIES
    DEPENDS                e2e_fn_ptr_compile
    PASS_REGULAR_EXPRESSION "fn_ptr_table OK")

# §A8 follow-up — typed vector tables (§14.5.3). The Vestra
# spelling `[N]@interrupt(T)` is a fixed-length array of ISR
# slots; each slot is an @interrupt-attributed function with
# shape `(inout T) -> Unit`. Codegen emits a `std::array<void(
# *)(T&), N>` with a brace-init list of function names that
# decay to function pointers at the C++ layer. The runtime
# dispatches manually through the table by index, mimicking
# what hardware does on a real exception vector entry, and
# verifies each slot fires the right handler.
set(e2e_vec_table_input  ${CMAKE_SOURCE_DIR}/examples/vec_table.vst)
set(e2e_vec_table_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_vec_table.cpp)

add_test(
    NAME    e2e_vec_table_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_vec_table_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_vec_table_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_vec_table_driver} ${e2e_workdir}/vec_table.cpp
            -o ${e2e_workdir}/vec_table_app)
set_tests_properties(e2e_vec_table_compile PROPERTIES
    DEPENDS e2e_vec_table_transpile)

add_test(
    NAME    e2e_vec_table_run
    COMMAND ${e2e_workdir}/vec_table_app)
set_tests_properties(e2e_vec_table_run PROPERTIES
    DEPENDS                e2e_vec_table_compile
    PASS_REGULAR_EXPRESSION "vec_table OK")

# §A10 follow-up — memcpy / memset / memmove (§15.4). Three
# compiler-emitted intrinsics that route to __builtin_memcpy /
# __builtin_memset / __builtin_memmove. All three are gated on
# RawMemory, matching the audit lane the §A3 mints and §A5
# cache ops carry. The runtime exercises each over a stack-
# resident byte buffer + an overlapping range to prove memmove
# handles the in-place shift the kernel queue-compact path
# depends on.
set(e2e_mem_intrin_input  ${CMAKE_SOURCE_DIR}/examples/mem_intrinsics.vst)
set(e2e_mem_intrin_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_mem_intrinsics.cpp)

add_test(
    NAME    e2e_mem_intrin_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_mem_intrin_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_mem_intrin_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_mem_intrin_driver} ${e2e_workdir}/mem_intrinsics.cpp
            -o ${e2e_workdir}/mem_intrinsics_app)
set_tests_properties(e2e_mem_intrin_compile PROPERTIES
    DEPENDS e2e_mem_intrin_transpile)

add_test(
    NAME    e2e_mem_intrin_run
    COMMAND ${e2e_workdir}/mem_intrinsics_app)
set_tests_properties(e2e_mem_intrin_run PROPERTIES
    DEPENDS                e2e_mem_intrin_compile
    PASS_REGULAR_EXPRESSION "mem_intrinsics OK")

# §A11 follow-up — PerCpu.new + slot(hartId) (§14.8). The heap
# factory `PerCpu.new(value) using Alloc -> Box[PerCpu[T]]`
# lowers to std::make_unique<__vstr::PerCpu<T>>(...) and is
# gated on Alloc; the cross-hart accessor `.slot(hartId) using
# RawMemory -> Ptr[T]` lowers to the runtime PerCpu's `slot()`
# member (hosted: returns &value; kernel: indexes per-CPU
# array). The runtime exercises .mine() for the local-hart
# slot and PerCpu.new for the heap path; the slot() cross-
# hart accessor is covered by the codegen + capability unit
# tests (its dereference syntax routes through a higher-
# level helper that v0.5 doesn't yet ship).
set(e2e_pce_input  ${CMAKE_SOURCE_DIR}/examples/per_cpu_extras.vst)
set(e2e_pce_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_per_cpu_extras.cpp)

add_test(
    NAME    e2e_pce_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_pce_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_pce_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_pce_driver} ${e2e_workdir}/per_cpu_extras.cpp
            -o ${e2e_workdir}/per_cpu_extras_app)
set_tests_properties(e2e_pce_compile PROPERTIES
    DEPENDS e2e_pce_transpile)

add_test(
    NAME    e2e_pce_run
    COMMAND ${e2e_workdir}/per_cpu_extras_app)
set_tests_properties(e2e_pce_run PROPERTIES
    DEPENDS                e2e_pce_compile
    PASS_REGULAR_EXPRESSION "per_cpu_extras OK")

# §A3 follow-up — Ptr / MutPtr `.value` dereference. The
# spelling mirrors Box[T].value, lowers to (*p) at the C++
# layer, and unlocks the cross-hart slot() pattern as well
# as ordinary raw-pointer reads. The runtime stash-and-read
# verifies the read direction; store_word writes through a
# MutPtr; bump_word demonstrates the RMW shape.
set(e2e_ptr_deref_input  ${CMAKE_SOURCE_DIR}/examples/ptr_deref.vst)
set(e2e_ptr_deref_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_ptr_deref.cpp)

add_test(
    NAME    e2e_ptr_deref_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_ptr_deref_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_ptr_deref_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_ptr_deref_driver} ${e2e_workdir}/ptr_deref.cpp
            -o ${e2e_workdir}/ptr_deref_app)
set_tests_properties(e2e_ptr_deref_compile PROPERTIES
    DEPENDS e2e_ptr_deref_transpile)

add_test(
    NAME    e2e_ptr_deref_run
    COMMAND ${e2e_workdir}/ptr_deref_app)
set_tests_properties(e2e_ptr_deref_run PROPERTIES
    DEPENDS                e2e_ptr_deref_compile
    PASS_REGULAR_EXPRESSION "ptr_deref OK")

# §A9 first slice — @boot + @kernel_init (§14.7). The stage-1
# entry chain attributes: @boot for pre-MMU code (naked, no
# heap, no unwinder), @kernel_init for the transitional
# regime between boot and the scheduler's first swapContext.
# The runtime exercises the kernel_init -> kernel_main marker
# round-trip; @boot's body is empty on hosted (real kernels
# would put an asm block there) but its [[gnu::naked]]
# attribute survives to link time.
set(e2e_boot_chain_input  ${CMAKE_SOURCE_DIR}/examples/boot_chain.vst)
set(e2e_boot_chain_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_boot_chain.cpp)

add_test(
    NAME    e2e_boot_chain_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_boot_chain_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_boot_chain_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_boot_chain_driver} ${e2e_workdir}/boot_chain.cpp
            -o ${e2e_workdir}/boot_chain_app)
set_tests_properties(e2e_boot_chain_compile PROPERTIES
    DEPENDS e2e_boot_chain_transpile)

add_test(
    NAME    e2e_boot_chain_run
    COMMAND ${e2e_workdir}/boot_chain_app)
set_tests_properties(e2e_boot_chain_run PROPERTIES
    DEPENDS                e2e_boot_chain_compile
    PASS_REGULAR_EXPRESSION "boot_chain OK")

# §A11 follow-on — Padded[T] cache-line wrapper (§14.8).
# The wrapper rounds sizeof up to a cache line and aligns
# storage on a cache-line boundary; the false-sharing-
# resistance property is enforced via static_assert in the
# driver. Runtime tests round-trip Padded[UInt64] via .value
# and bump a Padded[Atomic[UInt32]] counter.
set(e2e_padded_input  ${CMAKE_SOURCE_DIR}/examples/padded.vst)
set(e2e_padded_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_padded.cpp)

add_test(
    NAME    e2e_padded_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_padded_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_padded_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_padded_driver} ${e2e_workdir}/padded.cpp
            -o ${e2e_workdir}/padded_app)
set_tests_properties(e2e_padded_compile PROPERTIES
    DEPENDS e2e_padded_transpile)

add_test(
    NAME    e2e_padded_run
    COMMAND ${e2e_workdir}/padded_app)
set_tests_properties(e2e_padded_run PROPERTIES
    DEPENDS                e2e_padded_compile
    PASS_REGULAR_EXPRESSION "padded OK")

# §8 follow-up — pattern-matching enhancements. Three shapes
# the prior slices left partial: where-guards on enum-payload
# binds, tuple patterns with wildcards, and let-pattern +
# guard combination. Runtime exercises payloaded-enum guards,
# bare-enum guards (forcing the if-chain lowering), and tuple
# wildcards across both first-match-wins and default fall-
# through.
set(e2e_mg_input  ${CMAKE_SOURCE_DIR}/examples/match_guards.vst)
set(e2e_mg_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_match_guards.cpp)

add_test(
    NAME    e2e_mg_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_mg_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_mg_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_mg_driver} ${e2e_workdir}/match_guards.cpp
            -o ${e2e_workdir}/match_guards_app)
set_tests_properties(e2e_mg_compile PROPERTIES
    DEPENDS e2e_mg_transpile)

add_test(
    NAME    e2e_mg_run
    COMMAND ${e2e_workdir}/match_guards_app)
set_tests_properties(e2e_mg_run PROPERTIES
    DEPENDS                e2e_mg_compile
    PASS_REGULAR_EXPRESSION "match_guards OK")

# §A10 follow-up — @stack_protector + --no-libc (§15.4 / §15.5).
# The runtime check builds hosted (its driver uses the C++ standard
# library, which a --no-libc header deliberately doesn't coexist with);
# a separate --no-libc transpile checks the freestanding marker. The
# freestanding compile itself is e2e_freestanding_stack_protector.
set(e2e_sp_input  ${CMAKE_SOURCE_DIR}/examples/stack_protector.vst)
set(e2e_sp_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_stack_protector.cpp)

add_test(
    NAME    e2e_sp_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_sp_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_sp_marker
    COMMAND sh -c "$<TARGET_FILE:vestra> build ${e2e_sp_input} -o ${e2e_workdir}/_sp_nolibc --no-libc >/dev/null && grep -q 'vestra: no_libc = true' ${e2e_workdir}/_sp_nolibc/stack_protector.hpp")

add_test(
    NAME    e2e_sp_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_sp_driver} ${e2e_workdir}/stack_protector.cpp
            -o ${e2e_workdir}/stack_protector_app)
set_tests_properties(e2e_sp_compile PROPERTIES
    DEPENDS e2e_sp_transpile)

add_test(
    NAME    e2e_sp_run
    COMMAND ${e2e_workdir}/stack_protector_app)
set_tests_properties(e2e_sp_run PROPERTIES
    DEPENDS                e2e_sp_compile
    PASS_REGULAR_EXPRESSION "stack_protector OK")

# §A6 last — @no_auto_barrier (§14.11.5 / §14.12.3). The
# kernel-init MMU bring-up escape from the per-write auto-
# barrier policy. Sema rejects the attribute outside a
# @kernel_init function; codegen accepts it silently (the
# Sysreg auto-barrier insertion queues for a later slice).
set(e2e_nab_input  ${CMAKE_SOURCE_DIR}/examples/no_auto_barrier.vst)
set(e2e_nab_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_no_auto_barrier.cpp)

add_test(
    NAME    e2e_nab_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_nab_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_nab_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_nab_driver} ${e2e_workdir}/no_auto_barrier.cpp
            -o ${e2e_workdir}/no_auto_barrier_app)
set_tests_properties(e2e_nab_compile PROPERTIES
    DEPENDS e2e_nab_transpile)

add_test(
    NAME    e2e_nab_run
    COMMAND ${e2e_workdir}/no_auto_barrier_app)
set_tests_properties(e2e_nab_run PROPERTIES
    DEPENDS                e2e_nab_compile
    PASS_REGULAR_EXPRESSION "no_auto_barrier OK")

# §14.12 first slice — typed Sysreg access. Sysreg.<name>
# resolves to a singleton in __vstr::sysreg with read/write
# methods; v0.5 hosted backs each register with a static cell
# so the runtime can verify round-trips. Auto-emitted post-
# write barriers from §14.12.3 queue for the second slice.
set(e2e_sr_input  ${CMAKE_SOURCE_DIR}/examples/sysreg.vst)
set(e2e_sr_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_sysreg.cpp)

add_test(
    NAME    e2e_sr_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_sr_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_sr_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_sr_driver} ${e2e_workdir}/sysreg.cpp
            -o ${e2e_workdir}/sysreg_app)
set_tests_properties(e2e_sr_compile PROPERTIES
    DEPENDS e2e_sr_transpile)

add_test(
    NAME    e2e_sr_run
    COMMAND ${e2e_workdir}/sysreg_app)
set_tests_properties(e2e_sr_run PROPERTIES
    DEPENDS                e2e_sr_compile
    PASS_REGULAR_EXPRESSION "sysreg OK")

# §14.12.3 / §15.5 audit surface (first slice). `vestra audit
# <file.vst> --sysreg` walks every Sysreg.<name>.{read|write}
# access in the unit and prints one site per line with the
# barrier-emission flag. The check verifies a representative
# call site from the existing sysreg.vst example — the gated
# vbar_el1 write outside @no_auto_barrier records [barrier:yes].
add_test(
    NAME    audit_sysreg_enumerator
    COMMAND $<TARGET_FILE:vestra> audit ${CMAKE_SOURCE_DIR}/examples/sysreg.vst --sysreg)
set_tests_properties(audit_sysreg_enumerator PROPERTIES
    PASS_REGULAR_EXPRESSION "Sysreg\\.vbar_el1\\.write \\[barrier:yes\\]")

# A second check confirms the @no_auto_barrier opt-out shows up
# as [barrier:no] on the same gated names. The enable_mmu fn
# in sysreg.vst carries @kernel_init @no_auto_barrier; its
# ttbr0_el1 + sctlr_el1 writes should report no barrier.
add_test(
    NAME    audit_sysreg_no_auto_barrier
    COMMAND $<TARGET_FILE:vestra> audit ${CMAKE_SOURCE_DIR}/examples/sysreg.vst --sysreg)
set_tests_properties(audit_sysreg_no_auto_barrier PROPERTIES
    PASS_REGULAR_EXPRESSION "Sysreg\\.ttbr0_el1\\.write \\[barrier:no\\]")

# §15.5 --no-libc enumerator. Walks the unit for @extern +
# @symbol declarations so the freestanding profile can list
# every external symbol the build emits. The link_attrs.vst
# example carries an `@symbol("kernel_entry_symbol")` decl
# that the enumerator surfaces.
add_test(
    NAME    audit_no_libc_lists_externs
    COMMAND $<TARGET_FILE:vestra> audit ${CMAKE_SOURCE_DIR}/examples/link_attrs.vst --no-libc)
set_tests_properties(audit_no_libc_lists_externs PROPERTIES
    PASS_REGULAR_EXPRESSION "extern kernel_entry_symbol")

# Zero-finding pass: the sysreg.vst example carries no @extern
# / @symbol decls, so --no-libc should produce no output and
# exit 0. We assert success via PASS_REGULAR_EXPRESSION on the
# empty-output sentinel: ctest's regex matches against the
# combined stdout/stderr, and the exit-status check is
# implicit (any non-zero exit fails the test).
add_test(
    NAME    audit_no_libc_clean_pass
    COMMAND $<TARGET_FILE:vestra> audit ${CMAKE_SOURCE_DIR}/examples/sysreg.vst --no-libc)
set_tests_properties(audit_no_libc_clean_pass PROPERTIES
    FAIL_REGULAR_EXPRESSION "extern ")

# §6 --safety enumerator. Walks every site granting an unsafe capability
# (RawMemory / Asm / Mmio) via a `using` row or `with` block and reports whether
# a `// Safety:` comment justifies it. safety_demo.vst documents some grants and
# leaves others bare, so the audit must surface both a yes and a no.
add_test(
    NAME    audit_safety_justified
    COMMAND $<TARGET_FILE:vestra> audit ${CMAKE_SOURCE_DIR}/examples/safety_demo.vst --safety)
set_tests_properties(audit_safety_justified PROPERTIES
    PASS_REGULAR_EXPRESSION "unsafe RawMemory \\[safety:yes\\]")

add_test(
    NAME    audit_safety_unjustified
    COMMAND $<TARGET_FILE:vestra> audit ${CMAKE_SOURCE_DIR}/examples/safety_demo.vst --safety)
set_tests_properties(audit_safety_unjustified PROPERTIES
    PASS_REGULAR_EXPRESSION "unsafe Asm \\[safety:no\\]")

# §14.9.4 wide-atomic target-feature gate (CLI end-to-end). The
# atomics.vst example uses Atomic[UInt128] + AtomicTaggedPointer;
# under --target=aarch64 without --target-features=lse2 the sema
# gate fires and the build refuses to emit. Conversely, supplying
# --target-features=lse2 lets the same build go through. The two
# tests pin the CLI plumbing + the sema feature-set check
# together so a regression in either layer is caught.
add_test(
    NAME    wide_atomic_target_gate_rejects_without_feature
    COMMAND $<TARGET_FILE:vestra> build ${CMAKE_SOURCE_DIR}/examples/atomics.vst
            -o ${e2e_workdir} --target=aarch64)
# PASS_REGULAR_EXPRESSION already says "the test passes if the
# pattern matches the output, regardless of exit status" — exactly
# what we want here, since the sema gate prints the typed
# diagnostic and exits non-zero.
set_tests_properties(wide_atomic_target_gate_rejects_without_feature PROPERTIES
    PASS_REGULAR_EXPRESSION "requires the 'lse2' target feature on aarch64")

add_test(
    NAME    wide_atomic_target_gate_admits_with_feature
    COMMAND $<TARGET_FILE:vestra> build ${CMAKE_SOURCE_DIR}/examples/atomics.vst
            -o ${e2e_workdir} --target=aarch64 --target-features=lse2)
set_tests_properties(wide_atomic_target_gate_admits_with_feature PROPERTIES
    FAIL_REGULAR_EXPRESSION "requires the 'lse2' target feature")

# Cache + TLB management (§A5 follow-up, §14.10.3, §14.10.4,
# §14.10.5). Nine free-function builtins for the data-cache /
# I-cache / TLB-invalidation ops. Hosted shims are no-ops except
# zeroData, which actually zeroes the span; the runtime cross-
# checks that side effect on a stack-resident byte buffer.
set(e2e_cache_tlb_input  ${CMAKE_SOURCE_DIR}/examples/cache_tlb.vst)
set(e2e_cache_tlb_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_cache_tlb.cpp)

add_test(
    NAME    e2e_cache_tlb_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_cache_tlb_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_cache_tlb_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_cache_tlb_driver} ${e2e_workdir}/cache_tlb.cpp
            -o ${e2e_workdir}/cache_tlb_app)
set_tests_properties(e2e_cache_tlb_compile PROPERTIES
    DEPENDS e2e_cache_tlb_transpile)

add_test(
    NAME    e2e_cache_tlb_run
    COMMAND ${e2e_workdir}/cache_tlb_app)
set_tests_properties(e2e_cache_tlb_run PROPERTIES
    DEPENDS                e2e_cache_tlb_compile
    PASS_REGULAR_EXPRESSION "cache_tlb OK")

# Raw-mint primitives (§A3, §10.5). Ptr / MutPtr / Span.raw /
# MutSpan.raw — all gated on RawMemory. Lowers to const T* / T*
# and std::span. The runtime feeds the address of a stack buffer
# in for hosted determinism, writes through MutSpan, and reads
# back via Span (the const path).
set(e2e_raw_mint_input  ${CMAKE_SOURCE_DIR}/examples/raw_mint.vst)
set(e2e_raw_mint_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_raw_mint.cpp)

add_test(
    NAME    e2e_raw_mint_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_raw_mint_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_raw_mint_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_raw_mint_driver} ${e2e_workdir}/raw_mint.cpp
            -o ${e2e_workdir}/raw_mint_app)
set_tests_properties(e2e_raw_mint_compile PROPERTIES
    DEPENDS e2e_raw_mint_transpile)

add_test(
    NAME    e2e_raw_mint_run
    COMMAND ${e2e_workdir}/raw_mint_app)
set_tests_properties(e2e_raw_mint_run PROPERTIES
    DEPENDS                e2e_raw_mint_compile
    PASS_REGULAR_EXPRESSION "raw_mint OK")

# Sync intrinsics (§A5, §14.10). Nine free-function builtins:
# compilerFence / memoryBarrier / syncBarrier / instructionBarrier /
# waitForInterrupt / waitForEvent / signalEvent / relax / nop.
# Codegen lowers each to a __vstr runtime shim; the runtime checks
# the calls don't crash and that a release-store + barrier round-
# trips correctly.
set(e2e_sync_intrinsics_input  ${CMAKE_SOURCE_DIR}/examples/sync_intrinsics.vst)
set(e2e_sync_intrinsics_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_sync_intrinsics.cpp)

add_test(
    NAME    e2e_sync_intrinsics_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_sync_intrinsics_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_sync_intrinsics_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_sync_intrinsics_driver} ${e2e_workdir}/sync_intrinsics.cpp
            -o ${e2e_workdir}/sync_intrinsics_app)
set_tests_properties(e2e_sync_intrinsics_compile PROPERTIES
    DEPENDS e2e_sync_intrinsics_transpile)

add_test(
    NAME    e2e_sync_intrinsics_run
    COMMAND ${e2e_workdir}/sync_intrinsics_app)
set_tests_properties(e2e_sync_intrinsics_run PROPERTIES
    DEPENDS                e2e_sync_intrinsics_compile
    PASS_REGULAR_EXPRESSION "sync_intrinsics OK")

# Atomic[T] (§A4, §14.9). Five orderings, load/store/exchange/
# fetchAdd/fetchSub. Lowers to std::atomic<T> + std::memory_order.
# The runtime resets, bumps, drains, and swaps a counter; flips a
# Bool flag. Cross-checks the final state via the C++ side reading
# the atomic directly.
set(e2e_atomics_input  ${CMAKE_SOURCE_DIR}/examples/atomics.vst)
set(e2e_atomics_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_atomics.cpp)

add_test(
    NAME    e2e_atomics_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_atomics_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_atomics_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_atomics_driver} ${e2e_workdir}/atomics.cpp
            -o ${e2e_workdir}/atomics_app)
set_tests_properties(e2e_atomics_compile PROPERTIES
    DEPENDS e2e_atomics_transpile)

add_test(
    NAME    e2e_atomics_run
    COMMAND ${e2e_workdir}/atomics_app)
set_tests_properties(e2e_atomics_run PROPERTIES
    DEPENDS                e2e_atomics_compile
    PASS_REGULAR_EXPRESSION "atomics OK")

# inline directive + layout reflection (§A2, §6.8 + §7.8).
# `@inline(.always|.never|.hint)` lowers to `[[gnu::always_inline]]
# inline` / `[[gnu::noinline]]` / `inline`. `T.size` / `T.alignment`
# fold at sema time to Int literals computed with C-style padding
# rules over primitives, vectors of primitives, and structs of those.
# The e2e cross-checks Vestra's fold against the host C++ ABI via
# static_assert on the matching std::size_t.
set(e2e_inline_layout_input  ${CMAKE_SOURCE_DIR}/examples/inline_layout.vst)
set(e2e_inline_layout_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_inline_layout.cpp)

add_test(
    NAME    e2e_inline_layout_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_inline_layout_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_inline_layout_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_inline_layout_driver} ${e2e_workdir}/inline_layout.cpp
            -o ${e2e_workdir}/inline_layout_app)
set_tests_properties(e2e_inline_layout_compile PROPERTIES
    DEPENDS e2e_inline_layout_transpile)

add_test(
    NAME    e2e_inline_layout_run
    COMMAND ${e2e_workdir}/inline_layout_app)
set_tests_properties(e2e_inline_layout_run PROPERTIES
    DEPENDS                e2e_inline_layout_compile
    PASS_REGULAR_EXPRESSION "inline_layout OK")

# Link attributes (§A1, §4.5 + §6.7). `@noinit static` reserves .bss
# storage with no initializer; `@section` / `@symbol` / `@weak` /
# `@alias` / `@visibility` lower to GNU attributes + asm-label
# trailers. The runtime confirms the .bss-resident byte reads back
# as zero, the renamed symbol still binds correctly, and the weak
# default's body runs.
set(e2e_link_attrs_input  ${CMAKE_SOURCE_DIR}/examples/link_attrs.vst)
set(e2e_link_attrs_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_link_attrs.cpp)

add_test(
    NAME    e2e_link_attrs_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_link_attrs_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_link_attrs_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_link_attrs_driver} ${e2e_workdir}/link_attrs.cpp
            -o ${e2e_workdir}/link_attrs_app)
set_tests_properties(e2e_link_attrs_compile PROPERTIES
    DEPENDS e2e_link_attrs_transpile)

add_test(
    NAME    e2e_link_attrs_run
    COMMAND ${e2e_workdir}/link_attrs_app)
set_tests_properties(e2e_link_attrs_run PROPERTIES
    DEPENDS                e2e_link_attrs_compile
    PASS_REGULAR_EXPRESSION "link_attrs OK")

# map / filter combinators: §9 builtins that take a closure for the
# per-element transform / predicate. Lower to __vstr::Map / __vstr::Filter
# via CTAD on both the iterator and the closure. Closures themselves
# emit as C++ `[&](T x) { return body; }` lambdas. The runtime
# combines map + filter + capture on a Stepper iterator.
set(e2e_map_filter_input  ${CMAKE_SOURCE_DIR}/examples/map_filter.vst)
set(e2e_map_filter_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_map_filter.cpp)

add_test(
    NAME    e2e_map_filter_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_map_filter_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_map_filter_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_map_filter_driver} ${e2e_workdir}/map_filter.cpp
            -o ${e2e_workdir}/map_filter_app)
set_tests_properties(e2e_map_filter_compile PROPERTIES
    DEPENDS e2e_map_filter_transpile)

add_test(
    NAME    e2e_map_filter_run
    COMMAND ${e2e_workdir}/map_filter_app)
set_tests_properties(e2e_map_filter_run PROPERTIES
    DEPENDS                e2e_map_filter_compile
    PASS_REGULAR_EXPRESSION "map_filter OK")

# Iterator combinators: §9 `zip(a, b)` / `take(xs, n)` builtins. Both
# adapt iterator-protocol structs into new iterators via __vstr::Zip
# / __vstr::Take in the runtime preamble; CTAD on each call site
# instantiates the template against the underlying iterator types.
# The runtime sums a zip, a take, and a take(zip(...)) composition.
set(e2e_iter_combinators_input  ${CMAKE_SOURCE_DIR}/examples/iter_combinators.vst)
set(e2e_iter_combinators_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_iter_combinators.cpp)

add_test(
    NAME    e2e_iter_combinators_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_iter_combinators_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_iter_combinators_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_iter_combinators_driver} ${e2e_workdir}/iter_combinators.cpp
            -o ${e2e_workdir}/iter_combinators_app)
set_tests_properties(e2e_iter_combinators_compile PROPERTIES
    DEPENDS e2e_iter_combinators_transpile)

add_test(
    NAME    e2e_iter_combinators_run
    COMMAND ${e2e_workdir}/iter_combinators_app)
set_tests_properties(e2e_iter_combinators_run PROPERTIES
    DEPENDS                e2e_iter_combinators_compile
    PASS_REGULAR_EXPRESSION "iter_combinators OK")

# for-in: §5 loop desugaring. Range forms lower to a C++ counted
# for-loop; user iterators (any value with `next() -> Element?`) lower
# to a while-true / next / break loop. Runtime sums an inclusive
# range, a half-open range, and a Countdown iterator to .none.
set(e2e_forin_input  ${CMAKE_SOURCE_DIR}/examples/forin.vst)
set(e2e_forin_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_forin.cpp)

add_test(
    NAME    e2e_forin_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_forin_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_forin_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_forin_driver} ${e2e_workdir}/forin.cpp
            -o ${e2e_workdir}/forin_app)
set_tests_properties(e2e_forin_compile PROPERTIES DEPENDS e2e_forin_transpile)

add_test(
    NAME    e2e_forin_run
    COMMAND ${e2e_workdir}/forin_app)
set_tests_properties(e2e_forin_run PROPERTIES
    DEPENDS                e2e_forin_compile
    PASS_REGULAR_EXPRESSION "forin OK")

# derive(Clone): §12.3 explicit copy method. The codegen emits a
# `T clone() const { return *this; }` on struct and sum-type enum
# wrappers; bare enums get no method (no slot in C++ enum class).
# Runtime exercises clone on a struct (mutating the copy, asserting the
# input is untouched) and on each Shape variant.
set(e2e_clone_input  ${CMAKE_SOURCE_DIR}/examples/derive_clone.vst)
set(e2e_clone_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_derive_clone.cpp)

add_test(
    NAME    e2e_derive_clone_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_clone_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_derive_clone_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_clone_driver} ${e2e_workdir}/derive_clone.cpp
            -o ${e2e_workdir}/derive_clone_app)
set_tests_properties(e2e_derive_clone_compile PROPERTIES DEPENDS e2e_derive_clone_transpile)

add_test(
    NAME    e2e_derive_clone_run
    COMMAND ${e2e_workdir}/derive_clone_app)
set_tests_properties(e2e_derive_clone_run PROPERTIES
    DEPENDS                e2e_derive_clone_compile
    PASS_REGULAR_EXPRESSION "derive\\(Clone\\) OK")

# do / catch: §9 inline error handling. The do-block is checked under
# the annotated catch's error type so any `try` inside propagates
# locally; the catch arm binds the error value and recovers. Lowers to
# nested IIFEs over std::expected. Runtime exercises success, error,
# multi-try-body, and the bound-error dispatch.
set(e2e_docatch_input  ${CMAKE_SOURCE_DIR}/examples/docatch.vst)
set(e2e_docatch_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_docatch.cpp)

add_test(
    NAME    e2e_docatch_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_docatch_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_docatch_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_docatch_driver} ${e2e_workdir}/docatch.cpp
            -o ${e2e_workdir}/docatch_app)
set_tests_properties(e2e_docatch_compile PROPERTIES DEPENDS e2e_docatch_transpile)

add_test(
    NAME    e2e_docatch_run
    COMMAND ${e2e_workdir}/docatch_app)
set_tests_properties(e2e_docatch_run PROPERTIES
    DEPENDS                e2e_docatch_compile
    PASS_REGULAR_EXPRESSION "docatch OK")

# Optional chaining: §9 `a?.b?.c` short-circuits to nullopt on any
# missing link. Each `?.` lowers to std::optional::transform (member
# returns T) or and_then (member returns Optional<U>, flattens). The
# runtime exercises every link's success/failure path.
set(e2e_optchain_input  ${CMAKE_SOURCE_DIR}/examples/optchain.vst)
set(e2e_optchain_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_optchain.cpp)

add_test(
    NAME    e2e_optchain_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_optchain_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_optchain_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_optchain_driver} ${e2e_workdir}/optchain.cpp
            -o ${e2e_workdir}/optchain_app)
set_tests_properties(e2e_optchain_compile PROPERTIES DEPENDS e2e_optchain_transpile)

add_test(
    NAME    e2e_optchain_run
    COMMAND ${e2e_workdir}/optchain_app)
set_tests_properties(e2e_optchain_run PROPERTIES
    DEPENDS                e2e_optchain_compile
    PASS_REGULAR_EXPRESSION "optchain OK")

# Result / throws / try / throw: §9 error handling. throws(E) lowers to
# std::expected<T, E>; try / try? / try! lower to propagation /
# Optional-conversion / .value() respectively. The runtime exercises all
# three forms across success and failure paths, including the
# std::bad_expected_access panic from try! on an error.
set(e2e_result_input  ${CMAKE_SOURCE_DIR}/examples/result.vst)
set(e2e_result_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_result.cpp)

add_test(
    NAME    e2e_result_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_result_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_result_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_result_driver} ${e2e_workdir}/result.cpp
            -o ${e2e_workdir}/result_app)
set_tests_properties(e2e_result_compile PROPERTIES DEPENDS e2e_result_transpile)

add_test(
    NAME    e2e_result_run
    COMMAND ${e2e_workdir}/result_app)
set_tests_properties(e2e_result_run PROPERTIES
    DEPENDS                e2e_result_compile
    PASS_REGULAR_EXPRESSION "result OK")

# derive(Debug): std::formatter<T> specialization renders the value
# structurally for std::format / std::println / string interpolation.
# Exercises all three shapes (struct, bare enum, payloaded enum) and
# byte-checks the rendered strings.
set(e2e_dbg_input  ${CMAKE_SOURCE_DIR}/examples/derive_debug.vst)
set(e2e_dbg_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_derive_debug.cpp)

add_test(
    NAME    e2e_derive_debug_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_dbg_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_derive_debug_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_dbg_driver} ${e2e_workdir}/derive_debug.cpp
            -o ${e2e_workdir}/derive_debug_app)
set_tests_properties(e2e_derive_debug_compile PROPERTIES DEPENDS e2e_derive_debug_transpile)

add_test(
    NAME    e2e_derive_debug_run
    COMMAND ${e2e_workdir}/derive_debug_app)
set_tests_properties(e2e_derive_debug_run PROPERTIES
    DEPENDS                e2e_derive_debug_compile
    PASS_REGULAR_EXPRESSION "derive\\(Debug\\) OK")

# §7 generics phase 2 (first slice): user-defined generic structs.
set(e2e_genstruct_input  ${CMAKE_SOURCE_DIR}/examples/generic_struct.vst)
set(e2e_genstruct_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_generic_struct.cpp)

add_test(
    NAME    e2e_generic_struct_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_genstruct_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_generic_struct_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_genstruct_driver} ${e2e_workdir}/generic_struct.cpp
            -o ${e2e_workdir}/generic_struct_app)
set_tests_properties(e2e_generic_struct_compile PROPERTIES DEPENDS e2e_generic_struct_transpile)

add_test(
    NAME    e2e_generic_struct_run
    COMMAND ${e2e_workdir}/generic_struct_app)
set_tests_properties(e2e_generic_struct_run PROPERTIES
    DEPENDS                e2e_generic_struct_compile
    PASS_REGULAR_EXPRESSION "generic_struct OK")

# §7 generics phase 2 (second slice): user-defined generic enums.
set(e2e_genenum_input  ${CMAKE_SOURCE_DIR}/examples/generic_enum.vst)
set(e2e_genenum_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_generic_enum.cpp)

add_test(
    NAME    e2e_generic_enum_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_genenum_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_generic_enum_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_genenum_driver} ${e2e_workdir}/generic_enum.cpp
            -o ${e2e_workdir}/generic_enum_app)
set_tests_properties(e2e_generic_enum_compile PROPERTIES DEPENDS e2e_generic_enum_transpile)

add_test(
    NAME    e2e_generic_enum_run
    COMMAND ${e2e_workdir}/generic_enum_app)
set_tests_properties(e2e_generic_enum_run PROPERTIES
    DEPENDS                e2e_generic_enum_compile
    PASS_REGULAR_EXPRESSION "generic_enum OK")

# §7 generics phase 2 (third slice): const generics.
set(e2e_constgen_input  ${CMAKE_SOURCE_DIR}/examples/const_generic.vst)
set(e2e_constgen_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_const_generic.cpp)

add_test(
    NAME    e2e_const_generic_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_constgen_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_const_generic_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_constgen_driver} ${e2e_workdir}/const_generic.cpp
            -o ${e2e_workdir}/const_generic_app)
set_tests_properties(e2e_const_generic_compile PROPERTIES DEPENDS e2e_const_generic_transpile)

add_test(
    NAME    e2e_const_generic_run
    COMMAND ${e2e_workdir}/const_generic_app)
set_tests_properties(e2e_const_generic_run PROPERTIES
    DEPENDS                e2e_const_generic_compile
    PASS_REGULAR_EXPRESSION "const_generic OK")

# §7 generics phase 2: protocol bounds (where-clauses).
set(e2e_bounds_input  ${CMAKE_SOURCE_DIR}/examples/generic_bounds.vst)
set(e2e_bounds_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_generic_bounds.cpp)

add_test(
    NAME    e2e_generic_bounds_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_bounds_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_generic_bounds_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_bounds_driver} ${e2e_workdir}/generic_bounds.cpp
            -o ${e2e_workdir}/generic_bounds_app)
set_tests_properties(e2e_generic_bounds_compile PROPERTIES DEPENDS e2e_generic_bounds_transpile)

add_test(
    NAME    e2e_generic_bounds_run
    COMMAND ${e2e_workdir}/generic_bounds_app)
set_tests_properties(e2e_generic_bounds_run PROPERTIES
    DEPENDS                e2e_generic_bounds_compile
    PASS_REGULAR_EXPRESSION "generic_bounds OK")

# §12.3 derives on generic types.
set(e2e_gderive_input  ${CMAKE_SOURCE_DIR}/examples/generic_derive.vst)
set(e2e_gderive_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_generic_derive.cpp)

add_test(
    NAME    e2e_generic_derive_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_gderive_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_generic_derive_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_gderive_driver} ${e2e_workdir}/generic_derive.cpp
            -o ${e2e_workdir}/generic_derive_app)
set_tests_properties(e2e_generic_derive_compile PROPERTIES DEPENDS e2e_generic_derive_transpile)

add_test(
    NAME    e2e_generic_derive_run
    COMMAND ${e2e_workdir}/generic_derive_app)
set_tests_properties(e2e_generic_derive_run PROPERTIES
    DEPENDS                e2e_generic_derive_compile
    PASS_REGULAR_EXPRESSION "generic_derive OK")

# §11 async (first slice): async func + await.
set(e2e_async_input  ${CMAKE_SOURCE_DIR}/examples/async_demo.vst)
set(e2e_async_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_async_demo.cpp)

add_test(
    NAME    e2e_async_demo_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_async_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_async_demo_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_async_driver} ${e2e_workdir}/async_demo.cpp
            -o ${e2e_workdir}/async_demo_app)
set_tests_properties(e2e_async_demo_compile PROPERTIES DEPENDS e2e_async_demo_transpile)

add_test(
    NAME    e2e_async_demo_run
    COMMAND ${e2e_workdir}/async_demo_app)
set_tests_properties(e2e_async_demo_run PROPERTIES
    DEPENDS                e2e_async_demo_compile
    PASS_REGULAR_EXPRESSION "async_demo OK")

# §11.2 parallel.
set(e2e_parallel_input  ${CMAKE_SOURCE_DIR}/examples/parallel_demo.vst)
set(e2e_parallel_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_parallel_demo.cpp)

add_test(
    NAME    e2e_parallel_demo_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_parallel_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_parallel_demo_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_parallel_driver} ${e2e_workdir}/parallel_demo.cpp
            -o ${e2e_workdir}/parallel_demo_app)
set_tests_properties(e2e_parallel_demo_compile PROPERTIES DEPENDS e2e_parallel_demo_transpile)

add_test(
    NAME    e2e_parallel_demo_run
    COMMAND ${e2e_workdir}/parallel_demo_app)
set_tests_properties(e2e_parallel_demo_run PROPERTIES
    DEPENDS                e2e_parallel_demo_compile
    PASS_REGULAR_EXPRESSION "parallel_demo OK")

# §11 Channel[T].
set(e2e_channel_input  ${CMAKE_SOURCE_DIR}/examples/channel_demo.vst)
set(e2e_channel_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_channel_demo.cpp)

add_test(
    NAME    e2e_channel_demo_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_channel_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_channel_demo_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_channel_driver} ${e2e_workdir}/channel_demo.cpp
            -o ${e2e_workdir}/channel_demo_app)
set_tests_properties(e2e_channel_demo_compile PROPERTIES DEPENDS e2e_channel_demo_transpile)

add_test(
    NAME    e2e_channel_demo_run
    COMMAND ${e2e_workdir}/channel_demo_app)
set_tests_properties(e2e_channel_demo_run PROPERTIES
    DEPENDS                e2e_channel_demo_compile
    PASS_REGULAR_EXPRESSION "channel_demo OK")

# §11 blocking channel receive (scheduler slice 2).
set(e2e_chansel_input  ${CMAKE_SOURCE_DIR}/examples/channel_select_demo.vst)
set(e2e_chansel_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_channel_select_demo.cpp)

add_test(
    NAME    e2e_channel_select_demo_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_chansel_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_channel_select_demo_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_chansel_driver} ${e2e_workdir}/channel_select_demo.cpp
            -o ${e2e_workdir}/channel_select_demo_app)
set_tests_properties(e2e_channel_select_demo_compile PROPERTIES
    DEPENDS e2e_channel_select_demo_transpile)

add_test(
    NAME    e2e_channel_select_demo_run
    COMMAND ${e2e_workdir}/channel_select_demo_app)
set_tests_properties(e2e_channel_select_demo_run PROPERTIES
    DEPENDS                e2e_channel_select_demo_compile
    PASS_REGULAR_EXPRESSION "channel_select_demo OK")

# §12.4 quote / splice (expression context).
set(e2e_quote_input  ${CMAKE_SOURCE_DIR}/examples/quote_demo.vst)
set(e2e_quote_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_quote_demo.cpp)

add_test(
    NAME    e2e_quote_demo_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_quote_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_quote_demo_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_quote_driver} ${e2e_workdir}/quote_demo.cpp
            -o ${e2e_workdir}/quote_demo_app)
set_tests_properties(e2e_quote_demo_compile PROPERTIES DEPENDS e2e_quote_demo_transpile)

add_test(
    NAME    e2e_quote_demo_run
    COMMAND ${e2e_workdir}/quote_demo_app)
set_tests_properties(e2e_quote_demo_run PROPERTIES
    DEPENDS                e2e_quote_demo_compile
    PASS_REGULAR_EXPRESSION "quote_demo OK")

# §12.4 `vestra expand` prints the program after declaration-macro expansion:
# the generated wrapper `Seed1_seed` is present (proving @seeded expanded) while
# the comptime macro definition itself has dropped out.
add_test(
    NAME    e2e_quote_demo_expand
    COMMAND $<TARGET_FILE:vestra> expand ${e2e_quote_input})
set_tests_properties(e2e_quote_demo_expand PROPERTIES
    PASS_REGULAR_EXPRESSION "func Seed1_seed"
    FAIL_REGULAR_EXPRESSION "comptime func seeded")

# §18.5 Vec[T] growable sequence (lowers to std::vector).
set(e2e_vec_input  ${CMAKE_SOURCE_DIR}/examples/vec_demo.vst)
set(e2e_vec_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_vec_demo.cpp)

add_test(
    NAME    e2e_vec_demo_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_vec_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_vec_demo_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_vec_driver} ${e2e_workdir}/vec_demo.cpp
            -o ${e2e_workdir}/vec_demo_app)
set_tests_properties(e2e_vec_demo_compile PROPERTIES DEPENDS e2e_vec_demo_transpile)

add_test(
    NAME    e2e_vec_demo_run
    COMMAND ${e2e_workdir}/vec_demo_app)
set_tests_properties(e2e_vec_demo_run PROPERTIES
    DEPENDS                e2e_vec_demo_compile
    PASS_REGULAR_EXPRESSION "vec_demo OK")

# §18.5 String owned growable string (lowers to std::string).
set(e2e_string_input  ${CMAKE_SOURCE_DIR}/examples/string_demo.vst)
set(e2e_string_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_string_demo.cpp)

add_test(
    NAME    e2e_string_demo_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_string_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_string_demo_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_string_driver} ${e2e_workdir}/string_demo.cpp
            -o ${e2e_workdir}/string_demo_app)
set_tests_properties(e2e_string_demo_compile PROPERTIES DEPENDS e2e_string_demo_transpile)

add_test(
    NAME    e2e_string_demo_run
    COMMAND ${e2e_workdir}/string_demo_app)
set_tests_properties(e2e_string_demo_run PROPERTIES
    DEPENDS                e2e_string_demo_compile
    PASS_REGULAR_EXPRESSION "string_demo OK")

# §18.5 String/Str read-only queries (slicing + search over std::string_view).
set(e2e_string_query_input  ${CMAKE_SOURCE_DIR}/examples/string_query_demo.vst)
set(e2e_string_query_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_string_query_demo.cpp)

add_test(
    NAME    e2e_string_query_demo_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_string_query_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_string_query_demo_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_string_query_driver} ${e2e_workdir}/string_query_demo.cpp
            -o ${e2e_workdir}/string_query_demo_app)
set_tests_properties(e2e_string_query_demo_compile PROPERTIES DEPENDS e2e_string_query_demo_transpile)

add_test(
    NAME    e2e_string_query_demo_run
    COMMAND ${e2e_workdir}/string_query_demo_app)
set_tests_properties(e2e_string_query_demo_run PROPERTIES
    DEPENDS                e2e_string_query_demo_compile
    PASS_REGULAR_EXPRESSION "string_query_demo OK")

# §18.5 number<->string conversion (toString rendering + toInt parsing).
set(e2e_string_convert_input  ${CMAKE_SOURCE_DIR}/examples/string_convert_demo.vst)
set(e2e_string_convert_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_string_convert_demo.cpp)

add_test(
    NAME    e2e_string_convert_demo_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_string_convert_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_string_convert_demo_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_string_convert_driver} ${e2e_workdir}/string_convert_demo.cpp
            -o ${e2e_workdir}/string_convert_demo_app)
set_tests_properties(e2e_string_convert_demo_compile PROPERTIES DEPENDS e2e_string_convert_demo_transpile)

add_test(
    NAME    e2e_string_convert_demo_run
    COMMAND ${e2e_workdir}/string_convert_demo_app)
set_tests_properties(e2e_string_convert_demo_run PROPERTIES
    DEPENDS                e2e_string_convert_demo_compile
    PASS_REGULAR_EXPRESSION "string_convert_demo OK")

# §18.5 String/Str.split(sep) → Vec[Str] tokenizer primitive.
set(e2e_string_split_input  ${CMAKE_SOURCE_DIR}/examples/string_split_demo.vst)
set(e2e_string_split_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_string_split_demo.cpp)

add_test(
    NAME    e2e_string_split_demo_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_string_split_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_string_split_demo_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_string_split_driver} ${e2e_workdir}/string_split_demo.cpp
            -o ${e2e_workdir}/string_split_demo_app)
set_tests_properties(e2e_string_split_demo_compile PROPERTIES DEPENDS e2e_string_split_demo_transpile)

add_test(
    NAME    e2e_string_split_demo_run
    COMMAND ${e2e_workdir}/string_split_demo_app)
set_tests_properties(e2e_string_split_demo_run PROPERTIES
    DEPENDS                e2e_string_split_demo_compile
    PASS_REGULAR_EXPRESSION "string_split_demo OK")

# §18.5 byte/char access for hand-tokenizing (charAt / chars / Char classifiers).
set(e2e_tokenize_input  ${CMAKE_SOURCE_DIR}/examples/tokenize_demo.vst)
set(e2e_tokenize_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_tokenize_demo.cpp)

add_test(
    NAME    e2e_tokenize_demo_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_tokenize_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_tokenize_demo_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_tokenize_driver} ${e2e_workdir}/tokenize_demo.cpp
            -o ${e2e_workdir}/tokenize_demo_app)
set_tests_properties(e2e_tokenize_demo_compile PROPERTIES DEPENDS e2e_tokenize_demo_transpile)

add_test(
    NAME    e2e_tokenize_demo_run
    COMMAND ${e2e_workdir}/tokenize_demo_app)
set_tests_properties(e2e_tokenize_demo_run PROPERTIES
    DEPENDS                e2e_tokenize_demo_compile
    PASS_REGULAR_EXPRESSION "tokenize_demo OK")

# §18.5 HashMap owned hash map (lowers to std::unordered_map<K, V>).
set(e2e_hashmap_input  ${CMAKE_SOURCE_DIR}/examples/hashmap_demo.vst)
set(e2e_hashmap_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_hashmap_demo.cpp)

add_test(
    NAME    e2e_hashmap_demo_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_hashmap_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_hashmap_demo_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_hashmap_driver} ${e2e_workdir}/hashmap_demo.cpp
            -o ${e2e_workdir}/hashmap_demo_app)
set_tests_properties(e2e_hashmap_demo_compile PROPERTIES DEPENDS e2e_hashmap_demo_transpile)

add_test(
    NAME    e2e_hashmap_demo_run
    COMMAND ${e2e_workdir}/hashmap_demo_app)
set_tests_properties(e2e_hashmap_demo_run PROPERTIES
    DEPENDS                e2e_hashmap_demo_compile
    PASS_REGULAR_EXPRESSION "hashmap_demo OK")

# §18.5 HashMap iteration (keys / values / entries -> Vec, for-(k,v) destructure).
set(e2e_hashmap_iter_input  ${CMAKE_SOURCE_DIR}/examples/hashmap_iter_demo.vst)
set(e2e_hashmap_iter_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_hashmap_iter_demo.cpp)

add_test(
    NAME    e2e_hashmap_iter_demo_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_hashmap_iter_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_hashmap_iter_demo_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_hashmap_iter_driver} ${e2e_workdir}/hashmap_iter_demo.cpp
            -o ${e2e_workdir}/hashmap_iter_demo_app)
set_tests_properties(e2e_hashmap_iter_demo_compile PROPERTIES DEPENDS e2e_hashmap_iter_demo_transpile)

add_test(
    NAME    e2e_hashmap_iter_demo_run
    COMMAND ${e2e_workdir}/hashmap_iter_demo_app)
set_tests_properties(e2e_hashmap_iter_demo_run PROPERTIES
    DEPENDS                e2e_hashmap_iter_demo_compile
    PASS_REGULAR_EXPRESSION "hashmap_iter_demo OK")

# §5 multi-file: `demo.app` (main.vst) imports `util.math` (util/math.vst). The
# transpile emits one .hpp/.cpp per module; the compile links both translation
# units; the run exercises a cross-module call and const.
set(e2e_mf_input  ${CMAKE_SOURCE_DIR}/examples/multifile_demo/main.vst)
set(e2e_mf_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_multifile_demo.cpp)

add_test(
    NAME    e2e_multifile_demo_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_mf_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_multifile_demo_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_mf_driver} ${e2e_workdir}/main.cpp ${e2e_workdir}/util/math.cpp
            -o ${e2e_workdir}/multifile_app)
set_tests_properties(e2e_multifile_demo_compile PROPERTIES
    DEPENDS e2e_multifile_demo_transpile)

add_test(
    NAME    e2e_multifile_demo_run
    COMMAND ${e2e_workdir}/multifile_app)
set_tests_properties(e2e_multifile_demo_run PROPERTIES
    DEPENDS                e2e_multifile_demo_compile
    PASS_REGULAR_EXPRESSION "multifile_demo OK")

# §5 imported enums and struct fields (F19): `examples/modenum_demo` imports
# `kern.ipc` and names its enum cases qualified and by leading dot, matches
# and catches over them, and reads fields typed by the module's own types.
set(e2e_me_input  ${CMAKE_SOURCE_DIR}/examples/modenum_demo/main.vst)
set(e2e_me_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_modenum_demo.cpp)
set(e2e_me_out    ${e2e_workdir}/_modenum)

add_test(
    NAME    e2e_modenum_demo_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_me_input} -o ${e2e_me_out})

add_test(
    NAME    e2e_modenum_demo_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_me_out}
            ${e2e_me_driver} ${e2e_me_out}/main.cpp ${e2e_me_out}/kern/ipc.cpp
            -o ${e2e_me_out}/modenum_app)
set_tests_properties(e2e_modenum_demo_compile PROPERTIES
    DEPENDS e2e_modenum_demo_transpile)

add_test(
    NAME    e2e_modenum_demo_run
    COMMAND ${e2e_me_out}/modenum_app)
set_tests_properties(e2e_modenum_demo_run PROPERTIES
    DEPENDS                e2e_modenum_demo_compile
    PASS_REGULAR_EXPRESSION "modenum_demo OK")

# §15.5 freestanding profile (F1/F2): `--no-libc` emits a prelude that needs
# no C++ standard library. Each example is transpiled with --no-libc and
# compiled for bare-metal aarch64 with -ffreestanding -nostdlibinc; the
# fs_<arch> probes are assembled per architecture so the lowered inline asm
# (barriers, cache/TLB maintenance, system registers) is checked too.
set(fs_workdir ${e2e_workdir}/_freestanding)
set(fs_flags_str "-ffreestanding -nostdlibinc -fno-exceptions -fno-rtti -std=c++26 -O2")
foreach(prog hello mmio mmio_wire mmio_union sysreg cache_tlb sync_intrinsics atomics
             interrupt_handler panic_handler panic link_attrs layout inline_layout padded per_cpu
             no_auto_barrier raw_mint mem_intrinsics stack_protector result docatch
             generic_enum match tuples optional spans chunks_demo iter_combinators derive_eq
             derive_hash)
    add_test(
        NAME    e2e_freestanding_${prog}
        COMMAND sh -c "rm -rf ${fs_workdir}/${prog} && $<TARGET_FILE:vestra> build ${CMAKE_SOURCE_DIR}/examples/${prog}.vst --no-libc -o ${fs_workdir}/${prog} >/dev/null && ${CMAKE_CXX_COMPILER} --target=aarch64-none-elf ${fs_flags_str} -c -I${fs_workdir}/${prog} ${fs_workdir}/${prog}/${prog}.cpp -o ${fs_workdir}/${prog}/${prog}.o")
endforeach()
# A compiler built without some backend (Apple clang has no RISC-V) can still
# parse for that target; check the prelude's C++ there and skip the assembler.
foreach(triple aarch64-none-elf x86_64-none-elf riscv64-none-elf)
    execute_process(
        COMMAND ${CMAKE_CXX_COMPILER} --target=${triple} -ffreestanding -x c++ -c /dev/null -o /dev/null
        RESULT_VARIABLE fs_probe_rc OUTPUT_QUIET ERROR_QUIET)
    if(fs_probe_rc EQUAL 0)
        set(fs_mode_${triple} "-c")
    else()
        set(fs_mode_${triple} "-fsyntax-only")
    endif()
endforeach()
foreach(probe "fs_a64|aarch64-none-elf|" "fs_asm|aarch64-none-elf|" "fs_x86|x86_64-none-elf|-minvpcid"
              "fs_rv|riscv64-none-elf|-march=rv64gc_zicbom_zicboz")
    string(REPLACE "|" ";" parts "${probe}")
    list(GET parts 0 prog)
    list(GET parts 1 triple)
    list(LENGTH parts nparts)
    set(extra "")
    if(nparts GREATER 2)
        list(GET parts 2 extra)
    endif()
    add_test(
        NAME    e2e_freestanding_${prog}
        COMMAND sh -c "rm -rf ${fs_workdir}/${prog} && $<TARGET_FILE:vestra> build ${CMAKE_SOURCE_DIR}/tests/e2e/freestanding/${prog}.vst --no-libc -o ${fs_workdir}/${prog} >/dev/null && ${CMAKE_CXX_COMPILER} --target=${triple} ${extra} ${fs_flags_str} ${fs_mode_${triple}} -I${fs_workdir}/${prog} ${fs_workdir}/${prog}/${prog}.cpp -o ${fs_workdir}/${prog}/${prog}.o")
endforeach()
# A register from another architecture is a compile error that says so,
# not a silent no-op.
add_test(
    NAME    e2e_freestanding_wrong_arch_sysreg
    COMMAND sh -c "rm -rf ${fs_workdir}/wrongarch && $<TARGET_FILE:vestra> build ${CMAKE_SOURCE_DIR}/examples/sysreg.vst --no-libc -o ${fs_workdir}/wrongarch >/dev/null && ${CMAKE_CXX_COMPILER} --target=x86_64-none-elf ${fs_flags_str} -fsyntax-only -I${fs_workdir}/wrongarch ${fs_workdir}/wrongarch/sysreg.cpp -o ${fs_workdir}/wrongarch/sysreg.o 2>&1; true")
set_tests_properties(e2e_freestanding_wrong_arch_sysreg PROPERTIES
    PASS_REGULAR_EXPRESSION "sctlr_el1 is an aarch64 register")

# §14.4 inline asm, run: examples/asm_demo uses unprivileged aarch64
# instructions only, so on an aarch64 host it builds hosted and executes.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64|ARM64)$")
    set(e2e_asm_out ${e2e_workdir}/_asm_demo)
    add_test(
        NAME    e2e_asm_demo_transpile
        COMMAND $<TARGET_FILE:vestra> build ${CMAKE_SOURCE_DIR}/examples/asm_demo/asm_demo.vst -o ${e2e_asm_out})
    add_test(
        NAME    e2e_asm_demo_compile
        COMMAND ${CMAKE_CXX_COMPILER} -std=c++26 -Wall -Wextra -O2 -I${e2e_asm_out}
                ${CMAKE_SOURCE_DIR}/tests/e2e/main_asm_demo.cpp ${e2e_asm_out}/asm_demo.cpp
                -o ${e2e_asm_out}/asm_demo_app)
    set_tests_properties(e2e_asm_demo_compile PROPERTIES DEPENDS e2e_asm_demo_transpile)
    add_test(NAME e2e_asm_demo_run COMMAND ${e2e_asm_out}/asm_demo_app)
    set_tests_properties(e2e_asm_demo_run PROPERTIES
        DEPENDS e2e_asm_demo_compile PASS_REGULAR_EXPRESSION "asm_demo OK")
endif()

# §14.4 a pinned register or clobber is checked against the --target ISA.
add_test(
    NAME    e2e_asm_bad_register
    COMMAND sh -c "printf 'func f(_ v: UInt64) using Asm {\\n    asm(in: [\\\"rax\\\": v], clobbers: [\\\"x31\\\"]) { \\\"nop\\\" }\\n}\\n' > ${e2e_workdir}/asm_badreg.vst && $<TARGET_FILE:vestra> build ${e2e_workdir}/asm_badreg.vst --target=aarch64 -o ${e2e_workdir}/_asm_badreg 2>&1; true")
set_tests_properties(e2e_asm_bad_register PROPERTIES
    PASS_REGULAR_EXPRESSION "'rax' is not a register on aarch64.*'x31' is not a register on aarch64")

# §14.12 under a named --target, another architecture's register is a Vestra
# error, not just a deleted function in the C++ (F4).
add_test(
    NAME    e2e_sysreg_wrong_target
    COMMAND sh -c "$<TARGET_FILE:vestra> build ${CMAKE_SOURCE_DIR}/tests/e2e/freestanding/fs_x86.vst --target=aarch64 -o ${e2e_workdir}/_sysreg_wrong 2>&1; true")
set_tests_properties(e2e_sysreg_wrong_target PROPERTIES
    PASS_REGULAR_EXPRESSION "Sysreg.ia32_efer is an x86_64 register; this build targets aarch64")

# §19.6 linear handles in storage (F17): take / put / borrowing if-let, run.
set(e2e_lin_out ${e2e_workdir}/_linear_demo)
add_test(
    NAME    e2e_linear_demo_transpile
    COMMAND $<TARGET_FILE:vestra> build ${CMAKE_SOURCE_DIR}/examples/linear_demo/linear_demo.vst -o ${e2e_lin_out})
add_test(
    NAME    e2e_linear_demo_compile
    COMMAND ${CMAKE_CXX_COMPILER} -std=c++26 -Wall -Wextra -O0 -I${e2e_lin_out}
            ${CMAKE_SOURCE_DIR}/tests/e2e/main_linear_demo.cpp ${e2e_lin_out}/linear_demo.cpp
            -o ${e2e_lin_out}/linear_demo_app)
set_tests_properties(e2e_linear_demo_compile PROPERTIES DEPENDS e2e_linear_demo_transpile)
add_test(NAME e2e_linear_demo_run COMMAND ${e2e_lin_out}/linear_demo_app)
set_tests_properties(e2e_linear_demo_run PROPERTIES
    DEPENDS e2e_linear_demo_compile PASS_REGULAR_EXPRESSION "linear_demo OK")
add_test(NAME e2e_linear_demo_put_occupied COMMAND sh -c "${e2e_lin_out}/linear_demo_app occupied 2>&1; true")
set_tests_properties(e2e_linear_demo_put_occupied PROPERTIES
    DEPENDS e2e_linear_demo_compile
    PASS_REGULAR_EXPRESSION "put into an Optional that already holds a value")
add_test(
    NAME    e2e_freestanding_linear_demo
    COMMAND sh -c "rm -rf ${fs_workdir}/linear_demo && $<TARGET_FILE:vestra> build ${CMAKE_SOURCE_DIR}/examples/linear_demo/linear_demo.vst --no-libc -o ${fs_workdir}/linear_demo >/dev/null && ${CMAKE_CXX_COMPILER} --target=aarch64-none-elf ${fs_flags_str} -c -I${fs_workdir}/linear_demo ${fs_workdir}/linear_demo/linear_demo.cpp -o ${fs_workdir}/linear_demo/linear_demo.o")

# The selfhost differential compares the fixtures' C++ as text, which says
# nothing about whether it compiles: compile each one (hosted, syntax only), so
# a fixture can't pass while its output is broken C++.
foreach(prog cg_func cg_expr cg_stmt cg_if cg_for cg_struct cg_index cgh_struct cgh_enum cgh_enumpl cg_enumcase cg_unwrap cg_match cg_enumctor cg_paymatch cg_matchdefault cg_matchguard cgh_enummethod cg_breakcont cgh_const cg_with cg_fnptr cg_closure cg_closurearg cg_matchpos cg_namedpl cg_print cg_mclosure cg_ifval cg_gpaymatch cg_builtins cg_lits cg_optchain cg_strco cg_interp cg_bmethods cg_genfunc cg_gentype cg_pmodes cg_throws cg_async cg_import cg_module cghelp cg_qualty cg_maperr cg_condhoist cg_matchhoist cg_docatch cg_gentail cg_amethod cg_channel cg_main cg_iflet cg_forproto cg_tpwild cg_paybindty cg_nestmatch cg_forpair cg_leaddot cg_valmatch cg_elseif cg_derive cg_derivespec cg_argsvec cg_opaque cg_box cg_moveonly cg_barecatch cg_hoistarms cg_tuplety cg_blockexpr cg_forelem cgh_global cg_orpat cg_withty cg_sinkarg cg_addrof cg_optflat cgh_requires cg_ifletval cgh_systypes cg_atomics cg_syncintr cg_sysstatic cg_linkattrs cgh_layout cgh_vectable cg_sysreg cg_rawmem cg_iteradapt cg_vecops cg_maperrclos cg_constgen cg_parallel cg_tupleparam cg_binprec cg_externabi cg_unitthrows)
    add_test(
        NAME    e2e_codegen_compiles_${prog}
        COMMAND sh -c "rm -rf ${cg_workdir}/compiles/${prog} && mkdir -p ${cg_workdir}/compiles/${prog} && $<TARGET_FILE:vestra> build ${cg_dir}/${prog}.vst -o ${cg_workdir}/compiles/${prog} >/dev/null && ${CMAKE_CXX_COMPILER} -std=c++26 -fsyntax-only -I${cg_workdir}/compiles/${prog} ${cg_workdir}/compiles/${prog}/${prog}.cpp")
endforeach()

# §14 `vestra audit --safety` lists Extern grants with the other unsafe ones.
add_test(
    NAME    e2e_audit_safety_extern
    COMMAND $<TARGET_FILE:vestra> audit ${CMAKE_SOURCE_DIR}/examples/cinterop_demo/cinterop.vst --safety)
set_tests_properties(e2e_audit_safety_extern PROPERTIES PASS_REGULAR_EXPRESSION "unsafe Extern")

# §14.5.3 a vector table over a frame without the canonical fields is an error.
add_test(
    NAME    e2e_vectable_frame_fields
    COMMAND sh -c "printf 'struct F {\\n    var pc: UInt64\\n}\\n@interrupt\\nfunc h(_ f: inout F) {\\n    f.pc = 0\\n}\\nstatic t: [2]@interrupt(F) = [h, h]\\n' > ${e2e_workdir}/vt_bad.vst && $<TARGET_FILE:vestra> build ${e2e_workdir}/vt_bad.vst --no-libc -o ${e2e_workdir}/_vt_bad 2>&1; true")
set_tests_properties(e2e_vectable_frame_fields PROPERTIES
    PASS_REGULAR_EXPRESSION "lacks the fields the compiler-emitted vector table saves into: x: \\[31\\]UInt64, sp: UInt64")

# §15.5 under --no-libc the hosted-runtime capabilities and heap types are
# Vestra errors (examples/boot_chain's `using Alloc` init needs a hosted heap).
add_test(
    NAME    e2e_nolibc_rejects_hosted
    COMMAND sh -c "$<TARGET_FILE:vestra> build ${CMAKE_SOURCE_DIR}/examples/boot_chain.vst --no-libc -o ${e2e_workdir}/_nolibc_bc 2>&1; true")
set_tests_properties(e2e_nolibc_rejects_hosted PROPERTIES
    PASS_REGULAR_EXPRESSION "`Alloc` needs the hosted runtime, which a --no-libc build")

# §8 / §9 Optional-yielding match, bindless cases, statement match with
# return / break, while let (F18), run.
set(e2e_mf18_out ${e2e_workdir}/_match_flow_demo)
add_test(
    NAME    e2e_match_flow_demo_transpile
    COMMAND $<TARGET_FILE:vestra> build ${CMAKE_SOURCE_DIR}/examples/match_flow_demo/match_flow_demo.vst -o ${e2e_mf18_out})
add_test(
    NAME    e2e_match_flow_demo_compile
    COMMAND ${CMAKE_CXX_COMPILER} -std=c++26 -Wall -Wextra -O0 -I${e2e_mf18_out}
            ${CMAKE_SOURCE_DIR}/tests/e2e/main_match_flow_demo.cpp ${e2e_mf18_out}/match_flow_demo.cpp
            -o ${e2e_mf18_out}/match_flow_demo_app)
set_tests_properties(e2e_match_flow_demo_compile PROPERTIES DEPENDS e2e_match_flow_demo_transpile)
add_test(NAME e2e_match_flow_demo_run COMMAND ${e2e_mf18_out}/match_flow_demo_app)
set_tests_properties(e2e_match_flow_demo_run PROPERTIES
    DEPENDS e2e_match_flow_demo_compile PASS_REGULAR_EXPRESSION "match_flow_demo OK")

# `vestra check` takes build's target options, so target-dependent errors
# (a register of another architecture, a hosted feature under --no-libc) match.
add_test(
    NAME    e2e_check_target_wrongarch
    COMMAND sh -c "$<TARGET_FILE:vestra> check ${CMAKE_SOURCE_DIR}/examples/sysreg.vst --target=x86_64 2>&1; true")
set_tests_properties(e2e_check_target_wrongarch PROPERTIES
    PASS_REGULAR_EXPRESSION "Sysreg.daif is an aarch64 register; this build targets x86_64")
add_test(
    NAME    e2e_check_nolibc
    COMMAND sh -c "$<TARGET_FILE:vestra> check ${CMAKE_SOURCE_DIR}/examples/boot_chain.vst --no-libc 2>&1; true")
set_tests_properties(e2e_check_nolibc PROPERTIES
    PASS_REGULAR_EXPRESSION "`Alloc` needs the hosted runtime")

# §15.5 F5: under --no-libc the @panic_handler is bound at link time through
# __vstr_panic_hook, not registered by a static initializer.
add_test(
    NAME    e2e_freestanding_panic_hook
    COMMAND sh -c "rm -rf ${e2e_workdir}/_fs_ph && $<TARGET_FILE:vestra> build ${CMAKE_SOURCE_DIR}/examples/panic_handler.vst --no-libc --target=aarch64 -o ${e2e_workdir}/_fs_ph >/dev/null && cat ${e2e_workdir}/_fs_ph/panic_handler.cpp")
set_tests_properties(e2e_freestanding_panic_hook PROPERTIES
    PASS_REGULAR_EXPRESSION "extern \"C\" void __vstr_panic_hook\\(.*\n    \\(void\\)vestraPanic\\(message, file, line\\);"
    FAIL_REGULAR_EXPRESSION "__vstr_register_panic_handler")

# §5 visibility: referencing a non-public (internal) decl of an imported module
# must fail to resolve. `vestra check` is expected to exit non-zero.
add_test(
    NAME    e2e_multifile_visibility
    COMMAND $<TARGET_FILE:vestra> check
            ${CMAKE_SOURCE_DIR}/examples/multifile_demo/badimport.vst)
set_tests_properties(e2e_multifile_visibility PROPERTIES WILL_FAIL TRUE)

# §5 cyclic import: cycle_a <-> cycle_b import each other; the loader reports a
# located cyclic-import error and `vestra check` exits non-zero.
add_test(
    NAME    e2e_cyclic_import
    COMMAND $<TARGET_FILE:vestra> check
            ${CMAKE_SOURCE_DIR}/examples/multifile_demo/cycle_a.vst)
set_tests_properties(e2e_cyclic_import PROPERTIES WILL_FAIL TRUE)

# §5 missing import: importing a module with no backing file is a located error
# at the `import`; `vestra check` exits non-zero.
add_test(
    NAME    e2e_missing_import
    COMMAND $<TARGET_FILE:vestra> check
            ${CMAKE_SOURCE_DIR}/examples/multifile_demo/missingimport.vst)
set_tests_properties(e2e_missing_import PROPERTIES WILL_FAIL TRUE)

# §5 C interop: `import c "clib.h"` emits a global-scope `#include "clib.h"`; an
# in-tree `@extern` binds to the inline cdemo_value() the header defines. The
# compile points -I at the example dir so the generated `#include` resolves.
set(e2e_cinterop_input  ${CMAKE_SOURCE_DIR}/examples/cinterop_demo/cinterop.vst)
set(e2e_cinterop_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_cinterop_demo.cpp)

add_test(
    NAME    e2e_cinterop_demo_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_cinterop_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_cinterop_demo_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            -I${CMAKE_SOURCE_DIR}/examples/cinterop_demo
            ${e2e_cinterop_driver} ${e2e_workdir}/cinterop.cpp
            -o ${e2e_workdir}/cinterop_app)
set_tests_properties(e2e_cinterop_demo_compile PROPERTIES DEPENDS e2e_cinterop_demo_transpile)

add_test(
    NAME    e2e_cinterop_demo_run
    COMMAND ${e2e_workdir}/cinterop_app)
set_tests_properties(e2e_cinterop_demo_run PROPERTIES
    DEPENDS                e2e_cinterop_demo_compile
    PASS_REGULAR_EXPRESSION "cinterop_demo OK")

# §5 import search roots: the entry imports `mathx`, whose file lives only under
# a separate -I root (lib/), not under the entry's own directory. The transpile
# resolves it via -I; the dependency emits at its import path (mathx.{hpp,cpp}).
set(e2e_sp_input  ${CMAKE_SOURCE_DIR}/examples/searchpath_demo/app/spmain.vst)
set(e2e_sp_root   ${CMAKE_SOURCE_DIR}/examples/searchpath_demo/lib)
set(e2e_sp_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_searchpath_demo.cpp)

add_test(
    NAME    e2e_searchpath_demo_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_sp_input} -I ${e2e_sp_root} -o ${e2e_workdir})

add_test(
    NAME    e2e_searchpath_demo_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_sp_driver} ${e2e_workdir}/spmain.cpp ${e2e_workdir}/mathx.cpp
            -o ${e2e_workdir}/searchpath_app)
set_tests_properties(e2e_searchpath_demo_compile PROPERTIES DEPENDS e2e_searchpath_demo_transpile)

add_test(
    NAME    e2e_searchpath_demo_run
    COMMAND ${e2e_workdir}/searchpath_app)
set_tests_properties(e2e_searchpath_demo_run PROPERTIES
    DEPENDS                e2e_searchpath_demo_compile
    PASS_REGULAR_EXPRESSION "searchpath_demo OK")

# §8 content-hashed @embed manifest. Emit the manifest from a build, then build
# again enforcing it (round-trip must agree). A third build enforces a manifest
# with a deliberately-wrong hash and must fail (drift detection).
set(e2e_embed_input ${CMAKE_SOURCE_DIR}/examples/embed_demo/embed_demo.vst)

add_test(
    NAME    e2e_embed_manifest_emit
    COMMAND $<TARGET_FILE:vestra> build ${e2e_embed_input} -o ${e2e_workdir}
            --emit-embed-manifest ${e2e_workdir}/embed_demo.manifest)

add_test(
    NAME    e2e_embed_manifest_enforce
    COMMAND $<TARGET_FILE:vestra> build ${e2e_embed_input} -o ${e2e_workdir}
            --embed-manifest ${e2e_workdir}/embed_demo.manifest)
set_tests_properties(e2e_embed_manifest_enforce PROPERTIES DEPENDS e2e_embed_manifest_emit)

add_test(
    NAME    e2e_embed_manifest_drift
    COMMAND $<TARGET_FILE:vestra> build ${e2e_embed_input} -o ${e2e_workdir}
            --embed-manifest ${CMAKE_SOURCE_DIR}/examples/embed_demo/drift.manifest)
set_tests_properties(e2e_embed_manifest_drift PROPERTIES WILL_FAIL TRUE)

# §13 elementwise vector arithmetic on [N]T: a * x + y lowers to __vstr::vec_*
# helpers over std::array; the compile + run exercises +, *, and lane indexing.
set(e2e_simd_input  ${CMAKE_SOURCE_DIR}/examples/simd_demo.vst)
set(e2e_simd_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_simd_demo.cpp)

add_test(
    NAME    e2e_simd_demo_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_simd_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_simd_demo_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O2
            -I${e2e_workdir}
            ${e2e_simd_driver} ${e2e_workdir}/simd_demo.cpp
            -o ${e2e_workdir}/simd_app)
set_tests_properties(e2e_simd_demo_compile PROPERTIES DEPENDS e2e_simd_demo_transpile)

add_test(
    NAME    e2e_simd_demo_run
    COMMAND ${e2e_workdir}/simd_app)
set_tests_properties(e2e_simd_demo_run PROPERTIES
    DEPENDS                e2e_simd_demo_compile
    PASS_REGULAR_EXPRESSION "simd_demo OK")

# §13 Soa[T] struct-of-arrays: Soa[Point] lowers to a tuple-of-vectors;
# push scatters fields into columns, get gathers a row back, len counts rows.
set(e2e_soa_input  ${CMAKE_SOURCE_DIR}/examples/soa_demo.vst)
set(e2e_soa_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_soa_demo.cpp)

add_test(
    NAME    e2e_soa_demo_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_soa_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_soa_demo_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O2
            -I${e2e_workdir}
            ${e2e_soa_driver} ${e2e_workdir}/soa_demo.cpp
            -o ${e2e_workdir}/soa_app)
set_tests_properties(e2e_soa_demo_compile PROPERTIES DEPENDS e2e_soa_demo_transpile)

add_test(
    NAME    e2e_soa_demo_run
    COMMAND ${e2e_workdir}/soa_app)
set_tests_properties(e2e_soa_demo_run PROPERTIES
    DEPENDS                e2e_soa_demo_compile
    PASS_REGULAR_EXPRESSION "soa_demo OK")

# §18 stdout I/O: print/println lower to std::print/std::println; `func main`
# is the real entry point, so io_demo compiles standalone (no driver) and its
# own output is the assertion.
set(e2e_io_input ${CMAKE_SOURCE_DIR}/examples/io_demo.vst)

add_test(
    NAME    e2e_io_demo_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_io_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_io_demo_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_workdir}/io_demo.cpp
            -o ${e2e_workdir}/io_app)
set_tests_properties(e2e_io_demo_compile PROPERTIES DEPENDS e2e_io_demo_transpile)

add_test(
    NAME    e2e_io_demo_run
    COMMAND ${e2e_workdir}/io_app)
set_tests_properties(e2e_io_demo_run PROPERTIES
    DEPENDS                e2e_io_demo_compile
    PASS_REGULAR_EXPRESSION "Hello, Vestra!")

# §18 stderr output: eprint/eprintln lower to std::print(stderr, ...). Standalone
# (`func main`). Two redirected run steps prove the stream routing — keeping only
# stdout shows the OUT line but not the ERR diagnostics, and keeping only stderr
# shows the reverse.
set(e2e_eprint_input ${CMAKE_SOURCE_DIR}/examples/eprint_demo.vst)

add_test(
    NAME    e2e_eprint_demo_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_eprint_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_eprint_demo_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_workdir}/eprint_demo.cpp
            -o ${e2e_workdir}/eprint_app)
set_tests_properties(e2e_eprint_demo_compile PROPERTIES DEPENDS e2e_eprint_demo_transpile)

# Keep only stdout: the OUT line is present, the ERR diagnostics are not.
add_test(
    NAME    e2e_eprint_demo_run_stdout
    COMMAND sh -c "${e2e_workdir}/eprint_app 2>/dev/null")
set_tests_properties(e2e_eprint_demo_run_stdout PROPERTIES
    DEPENDS                 e2e_eprint_demo_compile
    PASS_REGULAR_EXPRESSION "OUT:hello"
    FAIL_REGULAR_EXPRESSION "ERR:")

# Keep only stderr: the ERR diagnostics are present (eprint + eprintln join one
# line), the OUT line is not.
add_test(
    NAME    e2e_eprint_demo_run_stderr
    COMMAND sh -c "${e2e_workdir}/eprint_app 1>/dev/null")
set_tests_properties(e2e_eprint_demo_run_stderr PROPERTIES
    DEPENDS                 e2e_eprint_demo_compile
    PASS_REGULAR_EXPRESSION "ERR:partial"
    FAIL_REGULAR_EXPRESSION "OUT:")

# §18 filesystem I/O: writeFile then readFile round-trip, printed. Standalone
# (`func main`), so the program's own output is the assertion.
set(e2e_fileio_input ${CMAKE_SOURCE_DIR}/examples/fileio_demo.vst)

add_test(
    NAME    e2e_fileio_demo_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_fileio_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_fileio_demo_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_workdir}/fileio_demo.cpp
            -o ${e2e_workdir}/fileio_app)
set_tests_properties(e2e_fileio_demo_compile PROPERTIES DEPENDS e2e_fileio_demo_transpile)

add_test(
    NAME    e2e_fileio_demo_run
    COMMAND ${e2e_workdir}/fileio_app)
set_tests_properties(e2e_fileio_demo_run PROPERTIES
    DEPENDS                e2e_fileio_demo_compile
    PASS_REGULAR_EXPRESSION "read back: vestra file io")

# §18 command-line args: a module-less `func main` captures argc/argv, and
# `args()` reads them. The run passes args and the program echoes each one.
set(e2e_args_input ${CMAKE_SOURCE_DIR}/examples/args_demo.vst)

add_test(
    NAME    e2e_args_demo_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_args_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_args_demo_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_workdir}/args_demo.cpp
            -o ${e2e_workdir}/args_app)
set_tests_properties(e2e_args_demo_compile PROPERTIES DEPENDS e2e_args_demo_transpile)

add_test(
    NAME    e2e_args_demo_run
    COMMAND ${e2e_workdir}/args_app alpha beta)
set_tests_properties(e2e_args_demo_run PROPERTIES
    DEPENDS                e2e_args_demo_compile
    PASS_REGULAR_EXPRESSION "arg: beta")

# Self-hosting milestone: a real lexer for a small expression language, written
# in Vestra. Reads source (argv + readFile), scans it by character into a token
# stream (enum + struct + Vec), writes a "<KIND> <lexeme>" listing (writeFile),
# prints a count (stdout), and reports unexpected characters (eprint -> stderr).
set(e2e_lexer_input   ${CMAKE_SOURCE_DIR}/examples/lexer_demo.vst)
set(e2e_lexer_fixture ${CMAKE_SOURCE_DIR}/tests/e2e/lexer_input.txt)
set(e2e_lexer_bad     ${CMAKE_SOURCE_DIR}/tests/e2e/lexer_bad.txt)

add_test(
    NAME    e2e_lexer_demo_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_lexer_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_lexer_demo_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_workdir}/lexer_demo.cpp
            -o ${e2e_workdir}/lexer_app)
set_tests_properties(e2e_lexer_demo_compile PROPERTIES DEPENDS e2e_lexer_demo_transpile)

# Clean source: 12 tokens, summary on stdout.
add_test(
    NAME    e2e_lexer_demo_run
    COMMAND ${e2e_workdir}/lexer_app ${e2e_lexer_fixture} ${e2e_workdir}/lexer_out.txt)
set_tests_properties(e2e_lexer_demo_run PROPERTIES
    DEPENDS                 e2e_lexer_demo_compile
    PASS_REGULAR_EXPRESSION "tokens: 12")

# Verify the written token listing directly.
add_test(
    NAME    e2e_lexer_demo_verify
    COMMAND grep -F "IDENT foo3" ${e2e_workdir}/lexer_out.txt)
set_tests_properties(e2e_lexer_demo_verify PROPERTIES DEPENDS e2e_lexer_demo_run)

# Bad source: the unexpected '@' is reported on stderr (keep only stderr).
add_test(
    NAME    e2e_lexer_demo_errors
    COMMAND sh -c "${e2e_workdir}/lexer_app ${e2e_lexer_bad} ${e2e_workdir}/lexer_bad_out.txt 1>/dev/null")
set_tests_properties(e2e_lexer_demo_errors PROPERTIES
    DEPENDS                 e2e_lexer_demo_compile
    PASS_REGULAR_EXPRESSION "lex error: unexpected '@'")

# Self-hosting phase 2: a recursive-descent expression parser written in Vestra.
# Mutually-recursive grammar functions thread the cursor through `(value, pos)`
# tuples; the driver checks precedence, associativity, parentheses, and nesting.
set(e2e_calc_input  ${CMAKE_SOURCE_DIR}/examples/calc_demo.vst)
set(e2e_calc_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_calc_demo.cpp)

add_test(
    NAME    e2e_calc_demo_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_calc_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_calc_demo_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_calc_driver} ${e2e_workdir}/calc_demo.cpp
            -o ${e2e_workdir}/calc_demo_app)
set_tests_properties(e2e_calc_demo_compile PROPERTIES DEPENDS e2e_calc_demo_transpile)

add_test(
    NAME    e2e_calc_demo_run
    COMMAND ${e2e_workdir}/calc_demo_app)
set_tests_properties(e2e_calc_demo_run PROPERTIES
    DEPENDS                 e2e_calc_demo_compile
    PASS_REGULAR_EXPRESSION "calc_demo OK")

# Self-hosting phase 3: a materialized AST — a recursive enum whose binary case
# Boxes its children (the move-only ADT shape). The parser threads its cursor via
# inout and returns nodes by value; the driver checks both evaluation and a
# fully-parenthesized render that makes the tree structure visible.
set(e2e_calc_ast_input  ${CMAKE_SOURCE_DIR}/examples/calc_ast_demo.vst)
set(e2e_calc_ast_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_calc_ast_demo.cpp)

add_test(
    NAME    e2e_calc_ast_demo_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_calc_ast_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_calc_ast_demo_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_calc_ast_driver} ${e2e_workdir}/calc_ast_demo.cpp
            -o ${e2e_workdir}/calc_ast_demo_app)
set_tests_properties(e2e_calc_ast_demo_compile PROPERTIES DEPENDS e2e_calc_ast_demo_transpile)

add_test(
    NAME    e2e_calc_ast_demo_run
    COMMAND ${e2e_workdir}/calc_ast_demo_app)
set_tests_properties(e2e_calc_ast_demo_run PROPERTIES
    DEPENDS                 e2e_calc_ast_demo_compile
    PASS_REGULAR_EXPRESSION "calc_ast_demo OK")

# A move-only recursive list: a tuple-returning builder (move-on-consume) folded
# by mixed-arm matches (a bare-literal base case beside an intptr_t arm — the
# match-result-type fix). Exercises both codegen fixes together end to end.
set(e2e_movelist_input  ${CMAKE_SOURCE_DIR}/examples/movelist_demo.vst)
set(e2e_movelist_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_movelist_demo.cpp)

add_test(
    NAME    e2e_movelist_demo_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_movelist_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_movelist_demo_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_movelist_driver} ${e2e_workdir}/movelist_demo.cpp
            -o ${e2e_workdir}/movelist_demo_app)
set_tests_properties(e2e_movelist_demo_compile PROPERTIES DEPENDS e2e_movelist_demo_transpile)

add_test(
    NAME    e2e_movelist_demo_run
    COMMAND ${e2e_workdir}/movelist_demo_app)
set_tests_properties(e2e_movelist_demo_run PROPERTIES
    DEPENDS                 e2e_movelist_demo_compile
    PASS_REGULAR_EXPRESSION "movelist_demo OK")

# A two-phase front-end written in Vestra: a lexer produces a Vec[Token] stream,
# a recursive-descent parser consumes that stream into a Box[Ast] tree, and an
# evaluator folds it (lex -> tokens -> parse -> AST -> eval). The capstone of the
# self-hosting arc — char toolkit, Vec stream, move-only recursive AST, enum
# dispatch, and mixed-arm folds all in one program.
set(e2e_frontend_input  ${CMAKE_SOURCE_DIR}/examples/frontend_demo.vst)
set(e2e_frontend_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_frontend_demo.cpp)

add_test(
    NAME    e2e_frontend_demo_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_frontend_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_frontend_demo_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_frontend_driver} ${e2e_workdir}/frontend_demo.cpp
            -o ${e2e_workdir}/frontend_demo_app)
set_tests_properties(e2e_frontend_demo_compile PROPERTIES DEPENDS e2e_frontend_demo_transpile)

add_test(
    NAME    e2e_frontend_demo_run
    COMMAND ${e2e_workdir}/frontend_demo_app)
set_tests_properties(e2e_frontend_demo_run PROPERTIES
    DEPENDS                 e2e_frontend_demo_compile
    PASS_REGULAR_EXPRESSION "frontend_demo OK")

# Parse-error reporting: a token-stream parser written with §9 throws(ParseError)
# + try, so malformed input unwinds to a typed error. The driver inspects the
# std::expected each kind of bad input produces; evalOr exercises do/catch
# recovery. Building the move-only Box[Ast] through `try` exercises the move-on-
# consume extension into the §9 machinery.
set(e2e_parserr_input  ${CMAKE_SOURCE_DIR}/examples/parser_errors_demo.vst)
set(e2e_parserr_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_parser_errors_demo.cpp)

add_test(
    NAME    e2e_parser_errors_demo_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_parserr_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_parser_errors_demo_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_parserr_driver} ${e2e_workdir}/parser_errors_demo.cpp
            -o ${e2e_workdir}/parser_errors_demo_app)
set_tests_properties(e2e_parser_errors_demo_compile PROPERTIES DEPENDS e2e_parser_errors_demo_transpile)

add_test(
    NAME    e2e_parser_errors_demo_run
    COMMAND ${e2e_workdir}/parser_errors_demo_app)
set_tests_properties(e2e_parser_errors_demo_run PROPERTIES
    DEPENDS                 e2e_parser_errors_demo_compile
    PASS_REGULAR_EXPRESSION "parser_errors_demo OK")

# A variable-binding mini-language: the front-end grows a HashMap symbol table.
# `name = expr` statements bind names, later statements reference them. The token
# stream carries identifier names as Str slices; the env keys borrow the source.
set(e2e_calcvars_input  ${CMAKE_SOURCE_DIR}/examples/calc_vars_demo.vst)
set(e2e_calcvars_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_calc_vars_demo.cpp)

add_test(
    NAME    e2e_calc_vars_demo_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_calcvars_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_calc_vars_demo_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_calcvars_driver} ${e2e_workdir}/calc_vars_demo.cpp
            -o ${e2e_workdir}/calc_vars_demo_app)
set_tests_properties(e2e_calc_vars_demo_compile PROPERTIES DEPENDS e2e_calc_vars_demo_transpile)

add_test(
    NAME    e2e_calc_vars_demo_run
    COMMAND ${e2e_workdir}/calc_vars_demo_app)
set_tests_properties(e2e_calc_vars_demo_run PROPERTIES
    DEPENDS                 e2e_calc_vars_demo_compile
    PASS_REGULAR_EXPRESSION "calc_vars_demo OK")

# Control flow: the variable mini-language gains `if cond then a else b` and
# `<` / `>` comparisons (keywords recognized at lex time via a string match).
# The side-effect-free expressions let both branches evaluate and select.
set(e2e_calcif_input  ${CMAKE_SOURCE_DIR}/examples/calc_if_demo.vst)
set(e2e_calcif_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_calc_if_demo.cpp)

add_test(
    NAME    e2e_calc_if_demo_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_calcif_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_calc_if_demo_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_calcif_driver} ${e2e_workdir}/calc_if_demo.cpp
            -o ${e2e_workdir}/calc_if_demo_app)
set_tests_properties(e2e_calc_if_demo_compile PROPERTIES DEPENDS e2e_calc_if_demo_transpile)

add_test(
    NAME    e2e_calc_if_demo_run
    COMMAND ${e2e_workdir}/calc_if_demo_app)
set_tests_properties(e2e_calc_if_demo_run PROPERTIES
    DEPENDS                 e2e_calc_if_demo_compile
    PASS_REGULAR_EXPRESSION "calc_if_demo OK")

# User-defined (recursive) functions: `fn name(p) = body` is stored as its body's
# token-start index and re-run per call with a fresh binding; recursion works
# because the conditional is now lazy (a `live` flag skips the untaken branch).
# Exercises factorial and Fibonacci.
set(e2e_calcfn_input  ${CMAKE_SOURCE_DIR}/examples/calc_fn_demo.vst)
set(e2e_calcfn_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_calc_fn_demo.cpp)

add_test(
    NAME    e2e_calc_fn_demo_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_calcfn_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_calc_fn_demo_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_calcfn_driver} ${e2e_workdir}/calc_fn_demo.cpp
            -o ${e2e_workdir}/calc_fn_demo_app)
set_tests_properties(e2e_calc_fn_demo_compile PROPERTIES DEPENDS e2e_calc_fn_demo_transpile)

add_test(
    NAME    e2e_calc_fn_demo_run
    COMMAND ${e2e_workdir}/calc_fn_demo_app)
set_tests_properties(e2e_calc_fn_demo_run PROPERTIES
    DEPENDS                 e2e_calc_fn_demo_compile
    PASS_REGULAR_EXPRESSION "calc_fn_demo OK")

# Multi-argument functions: a comma-separated parameter list (stored as a
# Vec[Str] in HashMap[Str, Vec[Str]]) with positional argument binding. Exercises
# max / sum3 / two-argument recursion (power, gcd).
set(e2e_calcmfn_input  ${CMAKE_SOURCE_DIR}/examples/calc_multifn_demo.vst)
set(e2e_calcmfn_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_calc_multifn_demo.cpp)

add_test(
    NAME    e2e_calc_multifn_demo_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_calcmfn_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_calc_multifn_demo_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_calcmfn_driver} ${e2e_workdir}/calc_multifn_demo.cpp
            -o ${e2e_workdir}/calc_multifn_demo_app)
set_tests_properties(e2e_calc_multifn_demo_compile PROPERTIES DEPENDS e2e_calc_multifn_demo_transpile)

add_test(
    NAME    e2e_calc_multifn_demo_run
    COMMAND ${e2e_workdir}/calc_multifn_demo_app)
set_tests_properties(e2e_calc_multifn_demo_run PROPERTIES
    DEPENDS                 e2e_calc_multifn_demo_compile
    PASS_REGULAR_EXPRESSION "calc_multifn_demo OK")

# while loops: a statement layer with a mutable (inout) environment above the
# read-only expression layer. A loop re-runs its condition + body token ranges
# against the shared environment. Exercises iterative sum / factorial / power,
# nested loops, and a loop calling a function.
set(e2e_calcwhile_input  ${CMAKE_SOURCE_DIR}/examples/calc_while_demo.vst)
set(e2e_calcwhile_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_calc_while_demo.cpp)

add_test(
    NAME    e2e_calc_while_demo_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_calcwhile_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_calc_while_demo_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_calcwhile_driver} ${e2e_workdir}/calc_while_demo.cpp
            -o ${e2e_workdir}/calc_while_demo_app)
set_tests_properties(e2e_calc_while_demo_compile PROPERTIES DEPENDS e2e_calc_while_demo_transpile)

add_test(
    NAME    e2e_calc_while_demo_run
    COMMAND ${e2e_workdir}/calc_while_demo_app)
set_tests_properties(e2e_calc_while_demo_run PROPERTIES
    DEPENDS                 e2e_calc_while_demo_compile
    PASS_REGULAR_EXPRESSION "calc_while_demo OK")

# The interpreter as a standalone tool: a `func main` reads a program file from
# argv (args + readFile), runs the whole mini-language, and the program's `print`
# statements appear on stdout. The run executes a committed .calc program; the
# stdout assertion (fact(5) = 120, the loop's print) proves print + recursion +
# the file pipeline work together as an executable.
set(e2e_interp_input   ${CMAKE_SOURCE_DIR}/examples/interp_demo.vst)
set(e2e_interp_program ${CMAKE_SOURCE_DIR}/tests/e2e/interp_program.calc)

add_test(
    NAME    e2e_interp_demo_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_interp_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_interp_demo_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_workdir}/interp_demo.cpp
            -o ${e2e_workdir}/interp_app)
set_tests_properties(e2e_interp_demo_compile PROPERTIES DEPENDS e2e_interp_demo_transpile)

# Run the interpreter on the committed program. Its prints (120 from fact(5), 55
# from the loop, 7 from max) and the final `result: 7` go to stdout.
add_test(
    NAME    e2e_interp_demo_run
    COMMAND ${e2e_workdir}/interp_app ${e2e_interp_program})
set_tests_properties(e2e_interp_demo_run PROPERTIES
    DEPENDS                 e2e_interp_demo_compile
    PASS_REGULAR_EXPRESSION "result: 7")

# Verify a program `print` reached stdout (a recursive function's result),
# distinct from main's own `result:` echo.
add_test(
    NAME    e2e_interp_demo_verify
    COMMAND sh -c "${e2e_workdir}/interp_app ${e2e_interp_program} | grep -qx 120")
set_tests_properties(e2e_interp_demo_verify PROPERTIES DEPENDS e2e_interp_demo_compile)

# The tagged Value, threaded through the *full* standalone interpreter: the
# variable environment is now HashMap[Str, Value], so a variable can hold an int,
# a bool, or a string; `print` renders each by its tag; comparison / == produce
# bool; and `if` may pick between branches of different value types. The run
# executes a committed .calc program whose output proves a string variable
# (hello), recursion over Value (fact(6) = 720), a while-loop accumulation (55),
# and a heterogeneous `if` (big) all flow through the tagged value type.
set(e2e_interpval_input   ${CMAKE_SOURCE_DIR}/examples/interp_value_demo.vst)
set(e2e_interpval_program ${CMAKE_SOURCE_DIR}/tests/e2e/interp_value_program.calc)

add_test(
    NAME    e2e_interp_value_demo_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_interpval_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_interp_value_demo_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_workdir}/interp_value_demo.cpp
            -o ${e2e_workdir}/interp_value_app)
set_tests_properties(e2e_interp_value_demo_compile PROPERTIES DEPENDS e2e_interp_value_demo_transpile)

# Run the value interpreter on the committed program; its final `result: done`
# (the last statement is a string) goes to stdout.
add_test(
    NAME    e2e_interp_value_demo_run
    COMMAND ${e2e_workdir}/interp_value_app ${e2e_interpval_program})
set_tests_properties(e2e_interp_value_demo_run PROPERTIES
    DEPENDS                 e2e_interp_value_demo_compile
    PASS_REGULAR_EXPRESSION "result: done")

# Verify the value-typed outputs reached stdout: a string variable (hello) and
# recursion producing an int (720) — both rendered through the tagged Value.
add_test(
    NAME    e2e_interp_value_demo_verify
    COMMAND sh -c "${e2e_workdir}/interp_value_app ${e2e_interpval_program} | grep -qx hello && ${e2e_workdir}/interp_value_app ${e2e_interpval_program} | grep -qx 720")
set_tests_properties(e2e_interp_value_demo_verify PROPERTIES DEPENDS e2e_interp_value_demo_compile)

# String concatenation — the interpreter's first heap-owned value payload. The
# tagged Value's `str` case now holds an owned `String` (was a borrowed `Str`),
# and `+` over a string operand concatenates (each operand rendered by its tag).
# A fresh owned String rides through the variable environment and the Vec[Value]
# argument lists. The committed program proves literal + variable concatenation,
# string-led mixed concat ("n=" + 42), recursion that builds strings, and an
# owned String round-tripping through the env in a while loop.
set(e2e_interpcat_input   ${CMAKE_SOURCE_DIR}/examples/interp_concat_demo.vst)
set(e2e_interpcat_program ${CMAKE_SOURCE_DIR}/tests/e2e/interp_concat_program.calc)

add_test(
    NAME    e2e_interp_concat_demo_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_interpcat_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_interp_concat_demo_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_workdir}/interp_concat_demo.cpp
            -o ${e2e_workdir}/interp_concat_app)
set_tests_properties(e2e_interp_concat_demo_compile PROPERTIES DEPENDS e2e_interp_concat_demo_transpile)

# Run the concat interpreter on the committed program; final `result: done`.
add_test(
    NAME    e2e_interp_concat_demo_run
    COMMAND ${e2e_workdir}/interp_concat_app ${e2e_interpcat_program})
set_tests_properties(e2e_interp_concat_demo_run PROPERTIES
    DEPENDS                 e2e_interp_concat_demo_compile
    PASS_REGULAR_EXPRESSION "result: done")

# Verify the concatenation outputs reached stdout: a variable-interpolated concat
# (Hi Vestra!) and a recursion that builds an owned String (ababab).
add_test(
    NAME    e2e_interp_concat_demo_verify
    COMMAND sh -c "${e2e_workdir}/interp_concat_app ${e2e_interpcat_program} | grep -qx 'Hi Vestra!' && ${e2e_workdir}/interp_concat_app ${e2e_interpcat_program} | grep -qx ababab")
set_tests_properties(e2e_interp_concat_demo_verify PROPERTIES DEPENDS e2e_interp_concat_demo_compile)

# String builtins: `len` and `substr` over string values, dispatched in callFunc
# as a fallback for names not defined as `fn`s. `len` measures a string value,
# `substr(s, a, b)` builds a fresh owned String from the [a, b) sub-view (clamped).
# The committed program proves length, slicing, nested builtins (len(substr(...))),
# a user function calling a builtin, substring of a literal, and a clamped span.
set(e2e_interpstr_input   ${CMAKE_SOURCE_DIR}/examples/interp_strlib_demo.vst)
set(e2e_interpstr_program ${CMAKE_SOURCE_DIR}/tests/e2e/interp_strlib_program.calc)

add_test(
    NAME    e2e_interp_strlib_demo_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_interpstr_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_interp_strlib_demo_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_workdir}/interp_strlib_demo.cpp
            -o ${e2e_workdir}/interp_strlib_app)
set_tests_properties(e2e_interp_strlib_demo_compile PROPERTIES DEPENDS e2e_interp_strlib_demo_transpile)

# Run the strlib interpreter on the committed program; final `result: done`.
add_test(
    NAME    e2e_interp_strlib_demo_run
    COMMAND ${e2e_workdir}/interp_strlib_app ${e2e_interpstr_program})
set_tests_properties(e2e_interp_strlib_demo_run PROPERTIES
    DEPENDS                 e2e_interp_strlib_demo_compile
    PASS_REGULAR_EXPRESSION "result: done")

# Verify the builtin outputs reached stdout: a length (11) and a substring of a
# string literal (Vestra).
add_test(
    NAME    e2e_interp_strlib_demo_verify
    COMMAND sh -c "${e2e_workdir}/interp_strlib_app ${e2e_interpstr_program} | grep -qx 11 && ${e2e_workdir}/interp_strlib_app ${e2e_interpstr_program} | grep -qx Vestra")
set_tests_properties(e2e_interp_strlib_demo_verify PROPERTIES DEPENDS e2e_interp_strlib_demo_compile)

# Multi-value print: a `print` statement takes a comma-separated list of
# expressions and prints their rendered values on one line, space-separated. A
# single-value print is the one-element case (backward compatible). The committed
# program proves multi-value lines, expressions in the list, mixed value types
# per line, builtins in the list, and the single-value case.
set(e2e_interpprn_input   ${CMAKE_SOURCE_DIR}/examples/interp_print_demo.vst)
set(e2e_interpprn_program ${CMAKE_SOURCE_DIR}/tests/e2e/interp_print_program.calc)

add_test(
    NAME    e2e_interp_print_demo_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_interpprn_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_interp_print_demo_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_workdir}/interp_print_demo.cpp
            -o ${e2e_workdir}/interp_print_app)
set_tests_properties(e2e_interp_print_demo_compile PROPERTIES DEPENDS e2e_interp_print_demo_transpile)

# Run the multi-value print interpreter on the committed program; final result.
add_test(
    NAME    e2e_interp_print_demo_run
    COMMAND ${e2e_workdir}/interp_print_app ${e2e_interpprn_program})
set_tests_properties(e2e_interp_print_demo_run PROPERTIES
    DEPENDS                 e2e_interp_print_demo_compile
    PASS_REGULAR_EXPRESSION "result: done")

# Verify multi-value lines reached stdout: a labelled int pair (x = 10 y = 20)
# and a mixed string/bool/string line (mixed: true end).
add_test(
    NAME    e2e_interp_print_demo_verify
    COMMAND sh -c "${e2e_workdir}/interp_print_app ${e2e_interpprn_program} | grep -qx 'x = 10 y = 20' && ${e2e_workdir}/interp_print_app ${e2e_interpprn_program} | grep -qx 'mixed: true end'")
set_tests_properties(e2e_interp_print_demo_verify PROPERTIES DEPENDS e2e_interp_print_demo_compile)

# Search builtins: `find` and `contains` over string values, the search half of
# the string surface. `find(s, needle)` returns the first index or -1,
# `contains(s, needle)` a bool; both lower onto the synthesized String/Str search
# surface. The committed program proves find/contains, the absent and empty-needle
# cases, a user function calling a builtin, and a find+substr+len composition.
set(e2e_interpsrch_input   ${CMAKE_SOURCE_DIR}/examples/interp_search_demo.vst)
set(e2e_interpsrch_program ${CMAKE_SOURCE_DIR}/tests/e2e/interp_search_program.calc)

add_test(
    NAME    e2e_interp_search_demo_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_interpsrch_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_interp_search_demo_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_workdir}/interp_search_demo.cpp
            -o ${e2e_workdir}/interp_search_app)
set_tests_properties(e2e_interp_search_demo_compile PROPERTIES DEPENDS e2e_interp_search_demo_transpile)

# Run the search interpreter on the committed program; final `result: done`.
add_test(
    NAME    e2e_interp_search_demo_run
    COMMAND ${e2e_workdir}/interp_search_app ${e2e_interpsrch_program})
set_tests_properties(e2e_interp_search_demo_run PROPERTIES
    DEPENDS                 e2e_interp_search_demo_compile
    PASS_REGULAR_EXPRESSION "result: done")

# Verify search outputs reached stdout: a found index (find world: 6) and a
# four-builtin composition (compose: world).
add_test(
    NAME    e2e_interp_search_demo_verify
    COMMAND sh -c "${e2e_workdir}/interp_search_app ${e2e_interpsrch_program} | grep -qx 'find world: 6' && ${e2e_workdir}/interp_search_app ${e2e_interpsrch_program} | grep -qx 'compose: world'")
set_tests_properties(e2e_interp_search_demo_verify PROPERTIES DEPENDS e2e_interp_search_demo_compile)

# A string-keyed value map: the interpreter's first compound data type. The
# tagged Value gains a recursive `case map(HashMap[String, Value])` and four
# functional builtins (mapNew / put / get / has). The committed program proves
# string/int/bool values in a map, the absent-key default, a computed key (owned
# String keys), a nested map, and a frequency-count idiom.
set(e2e_interpmap_input   ${CMAKE_SOURCE_DIR}/examples/interp_map_demo.vst)
set(e2e_interpmap_program ${CMAKE_SOURCE_DIR}/tests/e2e/interp_map_program.calc)

add_test(
    NAME    e2e_interp_map_demo_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_interpmap_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_interp_map_demo_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_workdir}/interp_map_demo.cpp
            -o ${e2e_workdir}/interp_map_app)
set_tests_properties(e2e_interp_map_demo_compile PROPERTIES DEPENDS e2e_interp_map_demo_transpile)

# Run the map interpreter on the committed program; final `result: done`.
add_test(
    NAME    e2e_interp_map_demo_run
    COMMAND ${e2e_workdir}/interp_map_app ${e2e_interpmap_program})
set_tests_properties(e2e_interp_map_demo_run PROPERTIES
    DEPENDS                 e2e_interp_map_demo_compile
    PASS_REGULAR_EXPRESSION "result: done")

# Verify compound-value behavior reached stdout: a computed-key lookup
# (computed: 99) and a nested-map read (inner.x: 42).
add_test(
    NAME    e2e_interp_map_demo_verify
    COMMAND sh -c "${e2e_workdir}/interp_map_app ${e2e_interpmap_program} | grep -qx 'computed: 99' && ${e2e_workdir}/interp_map_app ${e2e_interpmap_program} | grep -qx 'inner.x: 42' && ${e2e_workdir}/interp_map_app ${e2e_interpmap_program} | grep -qx 'count a: 2'")
set_tests_properties(e2e_interp_map_demo_verify PROPERTIES DEPENDS e2e_interp_map_demo_compile)

# keys(m) and an ordered list value: the interpreter's second compound type. The
# tagged Value gains `case list(Vec[Value])`; keys(m) returns the map's keys as a
# list, at(xs, i) indexes it, and len now spans strings and lists. The committed
# program iterates a map by key (order-independent aggregates): a value sum, a
# threshold count, the empty-map case, and an out-of-range index.
set(e2e_interpkeys_input   ${CMAKE_SOURCE_DIR}/examples/interp_keys_demo.vst)
set(e2e_interpkeys_program ${CMAKE_SOURCE_DIR}/tests/e2e/interp_keys_program.calc)

add_test(
    NAME    e2e_interp_keys_demo_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_interpkeys_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_interp_keys_demo_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_workdir}/interp_keys_demo.cpp
            -o ${e2e_workdir}/interp_keys_app)
set_tests_properties(e2e_interp_keys_demo_compile PROPERTIES DEPENDS e2e_interp_keys_demo_transpile)

# Run the keys interpreter on the committed program; final `result: done`.
add_test(
    NAME    e2e_interp_keys_demo_run
    COMMAND ${e2e_workdir}/interp_keys_app ${e2e_interpkeys_program})
set_tests_properties(e2e_interp_keys_demo_run PROPERTIES
    DEPENDS                 e2e_interp_keys_demo_compile
    PASS_REGULAR_EXPRESSION "result: done")

# Verify list/keys behavior reached stdout: the key count (nkeys: 3) and a
# map-iteration value sum (total: 60), both order-independent.
add_test(
    NAME    e2e_interp_keys_demo_verify
    COMMAND sh -c "${e2e_workdir}/interp_keys_app ${e2e_interpkeys_program} | grep -qx 'nkeys: 3' && ${e2e_workdir}/interp_keys_app ${e2e_interpkeys_program} | grep -qx 'total: 60' && ${e2e_workdir}/interp_keys_app ${e2e_interpkeys_program} | grep -qx 'big: 2'")
set_tests_properties(e2e_interp_keys_demo_verify PROPERTIES DEPENDS e2e_interp_keys_demo_compile)

# split(s, sep) returning a list: a producer for the list value from strings. It
# cuts a string value into its pieces around a separator (piece count =
# occurrences + 1) and returns them as a list of string values, consumed by len /
# at. The committed program proves the piece count, indexed access, a deterministic
# rebuild-by-iteration, an empty-middle-piece edge case, and the no-separator case.
set(e2e_interpsplit_input   ${CMAKE_SOURCE_DIR}/examples/interp_split_demo.vst)
set(e2e_interpsplit_program ${CMAKE_SOURCE_DIR}/tests/e2e/interp_split_program.calc)

add_test(
    NAME    e2e_interp_split_demo_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_interpsplit_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_interp_split_demo_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_workdir}/interp_split_demo.cpp
            -o ${e2e_workdir}/interp_split_app)
set_tests_properties(e2e_interp_split_demo_compile PROPERTIES DEPENDS e2e_interp_split_demo_transpile)

# Run the split interpreter on the committed program; final `result: done`.
add_test(
    NAME    e2e_interp_split_demo_run
    COMMAND ${e2e_workdir}/interp_split_app ${e2e_interpsplit_program})
set_tests_properties(e2e_interp_split_demo_run PROPERTIES
    DEPENDS                 e2e_interp_split_demo_compile
    PASS_REGULAR_EXPRESSION "result: done")

# Verify split feeding the list type: a piece count (n: 4) and a deterministic
# rebuild-by-iteration over the pieces (joined: abcd).
add_test(
    NAME    e2e_interp_split_demo_verify
    COMMAND sh -c "${e2e_workdir}/interp_split_app ${e2e_interpsplit_program} | grep -qx 'n: 4' && ${e2e_workdir}/interp_split_app ${e2e_interpsplit_program} | grep -qx 'joined: abcd' && ${e2e_workdir}/interp_split_app ${e2e_interpsplit_program} | grep -qx 'edge: 3'")
set_tests_properties(e2e_interp_split_demo_verify PROPERTIES DEPENDS e2e_interp_split_demo_compile)

# join(list, sep): the inverse of split. It renders a list value's elements into
# one string with sep between each (n-1 separators), the consumer that turns the
# list type back into a string. The committed program proves join round-tripping
# with split, multi-char and empty separators, the singleton and empty-list cases,
# and the join(split(s, sep), sep) == s identity.
set(e2e_interpjoin_input   ${CMAKE_SOURCE_DIR}/examples/interp_join_demo.vst)
set(e2e_interpjoin_program ${CMAKE_SOURCE_DIR}/tests/e2e/interp_join_program.calc)

add_test(
    NAME    e2e_interp_join_demo_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_interpjoin_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_interp_join_demo_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_workdir}/interp_join_demo.cpp
            -o ${e2e_workdir}/interp_join_app)
set_tests_properties(e2e_interp_join_demo_compile PROPERTIES DEPENDS e2e_interp_join_demo_transpile)

# Run the join interpreter on the committed program; final `result: done`.
add_test(
    NAME    e2e_interp_join_demo_run
    COMMAND ${e2e_workdir}/interp_join_app ${e2e_interpjoin_program})
set_tests_properties(e2e_interp_join_demo_run PROPERTIES
    DEPENDS                 e2e_interp_join_demo_compile
    PASS_REGULAR_EXPRESSION "result: done")

# Verify join behavior reached stdout: a multi-char separator join
# (arrow: x -> y -> z) and the split/join round-trip identity (roundtrip: true).
add_test(
    NAME    e2e_interp_join_demo_verify
    COMMAND sh -c "${e2e_workdir}/interp_join_app ${e2e_interpjoin_program} | grep -qx 'arrow: x -> y -> z' && ${e2e_workdir}/interp_join_app ${e2e_interpjoin_program} | grep -qx 'roundtrip: true' && ${e2e_workdir}/interp_join_app ${e2e_interpjoin_program} | grep -qx 'comma: a,b,c,d'")
set_tests_properties(e2e_interp_join_demo_verify PROPERTIES DEPENDS e2e_interp_join_demo_compile)

# toInt(v): parse a string value's digits into an integer (distinct from the
# internal asInt coercion, which treats any string as 0). This closes the loop on
# numeric text: split a CSV of numbers, toInt each piece, sum them. The committed
# program proves a CSV sum and average, a plain parse, a negative, a malformed
# fallback to 0, int passthrough, and a bool conversion.
set(e2e_interptoint_input   ${CMAKE_SOURCE_DIR}/examples/interp_toint_demo.vst)
set(e2e_interptoint_program ${CMAKE_SOURCE_DIR}/tests/e2e/interp_toint_program.calc)

add_test(
    NAME    e2e_interp_toint_demo_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_interptoint_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_interp_toint_demo_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_workdir}/interp_toint_demo.cpp
            -o ${e2e_workdir}/interp_toint_app)
set_tests_properties(e2e_interp_toint_demo_compile PROPERTIES DEPENDS e2e_interp_toint_demo_transpile)

# Run the toInt interpreter on the committed program; final `result: done`.
add_test(
    NAME    e2e_interp_toint_demo_run
    COMMAND ${e2e_workdir}/interp_toint_app ${e2e_interptoint_program})
set_tests_properties(e2e_interp_toint_demo_run PROPERTIES
    DEPENDS                 e2e_interp_toint_demo_compile
    PASS_REGULAR_EXPRESSION "result: done")

# Verify numeric parsing reached stdout: the split + toInt CSV sum (total: 100), a
# negative parse (neg: -7), and the malformed fallback (bad: 0).
add_test(
    NAME    e2e_interp_toint_demo_verify
    COMMAND sh -c "${e2e_workdir}/interp_toint_app ${e2e_interptoint_program} | grep -qx 'total: 100' && ${e2e_workdir}/interp_toint_app ${e2e_interptoint_program} | grep -qx 'neg: -7' && ${e2e_workdir}/interp_toint_app ${e2e_interptoint_program} | grep -qx 'bad: 0'")
set_tests_properties(e2e_interp_toint_demo_verify PROPERTIES DEPENDS e2e_interp_toint_demo_compile)

# List builders: listNew / push / concat let a program build a list value directly
# (previously only keys / split produced one). Functional like put: push / concat
# copy the backing Vec, append, and return the new list. The committed program
# builds string and int lists element by element, concatenates them (order /
# mixed types preserved), accumulates a list in a loop, and concats a split result.
set(e2e_interplist_input   ${CMAKE_SOURCE_DIR}/examples/interp_listbuild_demo.vst)
set(e2e_interplist_program ${CMAKE_SOURCE_DIR}/tests/e2e/interp_listbuild_program.calc)

add_test(
    NAME    e2e_interp_listbuild_demo_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_interplist_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_interp_listbuild_demo_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_workdir}/interp_listbuild_demo.cpp
            -o ${e2e_workdir}/interp_listbuild_app)
set_tests_properties(e2e_interp_listbuild_demo_compile PROPERTIES DEPENDS e2e_interp_listbuild_demo_transpile)

# Run the list-builder interpreter on the committed program; final `result: done`.
add_test(
    NAME    e2e_interp_listbuild_demo_run
    COMMAND ${e2e_workdir}/interp_listbuild_app ${e2e_interplist_program})
set_tests_properties(e2e_interp_listbuild_demo_run PROPERTIES
    DEPENDS                 e2e_interp_listbuild_demo_compile
    PASS_REGULAR_EXPRESSION "result: done")

# Verify list building reached stdout: a mixed-type concat (cat: a,b,c,1,2,3) and
# a list accumulated in a loop (squares: 1 4 9 16).
add_test(
    NAME    e2e_interp_listbuild_demo_verify
    COMMAND sh -c "${e2e_workdir}/interp_listbuild_app ${e2e_interplist_program} | grep -qx 'cat: a,b,c,1,2,3' && ${e2e_workdir}/interp_listbuild_app ${e2e_interplist_program} | grep -qx 'squares: 1 4 9 16'")
set_tests_properties(e2e_interp_listbuild_demo_verify PROPERTIES DEPENDS e2e_interp_listbuild_demo_compile)

# toStr(v): render any value to a string value, the symmetric counterpart of
# toInt (it exposes the internal format as a builtin). Its payoff is numeric and
# computed map keys (put(m, toStr(i), v)) and stringified list elements. The
# committed program builds a map keyed by number strings, converts int/bool/string,
# builds a stringified list, and round-trips toInt(toStr(n)) == n.
set(e2e_interptostr_input   ${CMAKE_SOURCE_DIR}/examples/interp_tostr_demo.vst)
set(e2e_interptostr_program ${CMAKE_SOURCE_DIR}/tests/e2e/interp_tostr_program.calc)

add_test(
    NAME    e2e_interp_tostr_demo_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_interptostr_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_interp_tostr_demo_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_workdir}/interp_tostr_demo.cpp
            -o ${e2e_workdir}/interp_tostr_app)
set_tests_properties(e2e_interp_tostr_demo_compile PROPERTIES DEPENDS e2e_interp_tostr_demo_transpile)

# Run the toStr interpreter on the committed program; final `result: done`.
add_test(
    NAME    e2e_interp_tostr_demo_run
    COMMAND ${e2e_workdir}/interp_tostr_app ${e2e_interptostr_program})
set_tests_properties(e2e_interp_tostr_demo_run PROPERTIES
    DEPENDS                 e2e_interp_tostr_demo_compile
    PASS_REGULAR_EXPRESSION "result: done")

# Verify toStr behavior reached stdout: a number-keyed map lookup (sq3: 9), a
# stringified list (labels: 1,4,9), and the toInt/toStr round-trip (rt: 123).
add_test(
    NAME    e2e_interp_tostr_demo_verify
    COMMAND sh -c "${e2e_workdir}/interp_tostr_app ${e2e_interptostr_program} | grep -qx 'sq3: 9' && ${e2e_workdir}/interp_tostr_app ${e2e_interptostr_program} | grep -qx 'labels: 1,4,9' && ${e2e_workdir}/interp_tostr_app ${e2e_interptostr_program} | grep -qx 'rt: 123'")
set_tests_properties(e2e_interp_tostr_demo_verify PROPERTIES DEPENDS e2e_interp_tostr_demo_compile)

# sort(xs): sort a list value ascending (functional selection sort over Vec.get /
# Vec.set, type-aware comparison: ints numerically, strings lexicographically).
# This makes a keys list deterministic, so a map can finally be rendered in a
# stable order. The committed program proves sorted keys, a numeric sort (not
# lexicographic), a string sort, and the empty / singleton cases.
set(e2e_interpsort_input   ${CMAKE_SOURCE_DIR}/examples/interp_sort_demo.vst)
set(e2e_interpsort_program ${CMAKE_SOURCE_DIR}/tests/e2e/interp_sort_program.calc)

add_test(
    NAME    e2e_interp_sort_demo_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_interpsort_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_interp_sort_demo_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_workdir}/interp_sort_demo.cpp
            -o ${e2e_workdir}/interp_sort_app)
set_tests_properties(e2e_interp_sort_demo_compile PROPERTIES DEPENDS e2e_interp_sort_demo_transpile)

# Run the sort interpreter on the committed program; final `result: done`.
add_test(
    NAME    e2e_interp_sort_demo_run
    COMMAND ${e2e_workdir}/interp_sort_app ${e2e_interpsort_program})
set_tests_properties(e2e_interp_sort_demo_run PROPERTIES
    DEPENDS                 e2e_interp_sort_demo_compile
    PASS_REGULAR_EXPRESSION "result: done")

# Verify sorting reached stdout: deterministic sorted map keys
# (sorted keys: apple,banana,cherry) and a numeric (not lexicographic) sort
# (sorted nums: 5 10 20 30).
add_test(
    NAME    e2e_interp_sort_demo_verify
    COMMAND sh -c "${e2e_workdir}/interp_sort_app ${e2e_interpsort_program} | grep -qx 'sorted keys: apple,banana,cherry' && ${e2e_workdir}/interp_sort_app ${e2e_interpsort_program} | grep -qx 'sorted nums: 5 10 20 30'")
set_tests_properties(e2e_interp_sort_demo_verify PROPERTIES DEPENDS e2e_interp_sort_demo_compile)

# List reductions: sum / min / max fold a list value to a single value. sum coerces
# each element via toInt (so split string pieces sum directly); min / max reuse the
# type-aware lessVal ordering from sort. The committed program proves int sum / min
# / max, the average idiom, a CSV sum over split pieces, lexicographic min / max on
# a string list, and the empty-list cases.
set(e2e_interpreduce_input   ${CMAKE_SOURCE_DIR}/examples/interp_reduce_demo.vst)
set(e2e_interpreduce_program ${CMAKE_SOURCE_DIR}/tests/e2e/interp_reduce_program.calc)

add_test(
    NAME    e2e_interp_reduce_demo_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_interpreduce_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_interp_reduce_demo_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_workdir}/interp_reduce_demo.cpp
            -o ${e2e_workdir}/interp_reduce_app)
set_tests_properties(e2e_interp_reduce_demo_compile PROPERTIES DEPENDS e2e_interp_reduce_demo_transpile)

# Run the reduce interpreter on the committed program; final `result: done`.
add_test(
    NAME    e2e_interp_reduce_demo_run
    COMMAND ${e2e_workdir}/interp_reduce_app ${e2e_interpreduce_program})
set_tests_properties(e2e_interp_reduce_demo_run PROPERTIES
    DEPENDS                 e2e_interp_reduce_demo_compile
    PASS_REGULAR_EXPRESSION "result: done")

# Verify reductions reached stdout: an int sum (sum: 110), a CSV sum over split
# pieces (csv sum: 15), and a lexicographic max (max word: delta).
add_test(
    NAME    e2e_interp_reduce_demo_verify
    COMMAND sh -c "${e2e_workdir}/interp_reduce_app ${e2e_interpreduce_program} | grep -qx 'sum: 110' && ${e2e_workdir}/interp_reduce_app ${e2e_interpreduce_program} | grep -qx 'csv sum: 15' && ${e2e_workdir}/interp_reduce_app ${e2e_interpreduce_program} | grep -qx 'max word: delta'")
set_tests_properties(e2e_interp_reduce_demo_verify PROPERTIES DEPENDS e2e_interp_reduce_demo_compile)

# mapList(xs, "fn"): the interpreter's first higher-order operation. It applies a
# function (named by a string, resolved through callFunc, so a user fn or any
# builtin works) to each list element and collects the results. The committed
# program maps user functions (double / square) and builtins (len / toStr) over
# lists, and composes the result with sum / max / join / split.
set(e2e_interpmaplist_input   ${CMAKE_SOURCE_DIR}/examples/interp_maplist_demo.vst)
set(e2e_interpmaplist_program ${CMAKE_SOURCE_DIR}/tests/e2e/interp_maplist_program.calc)

add_test(
    NAME    e2e_interp_maplist_demo_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_interpmaplist_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_interp_maplist_demo_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_workdir}/interp_maplist_demo.cpp
            -o ${e2e_workdir}/interp_maplist_app)
set_tests_properties(e2e_interp_maplist_demo_compile PROPERTIES DEPENDS e2e_interp_maplist_demo_transpile)

# Run the mapList interpreter on the committed program; final `result: done`.
add_test(
    NAME    e2e_interp_maplist_demo_run
    COMMAND ${e2e_workdir}/interp_maplist_app ${e2e_interpmaplist_program})
set_tests_properties(e2e_interp_maplist_demo_run PROPERTIES
    DEPENDS                 e2e_interp_maplist_demo_compile
    PASS_REGULAR_EXPRESSION "result: done")

# Verify the higher-order transform reached stdout: a user function mapped over a
# list (squared: 1 4 9 16) and a builtin mapped over a list (lengths: 1,2,3).
add_test(
    NAME    e2e_interp_maplist_demo_verify
    COMMAND sh -c "${e2e_workdir}/interp_maplist_app ${e2e_interpmaplist_program} | grep -qx 'squared: 1 4 9 16' && ${e2e_workdir}/interp_maplist_app ${e2e_interpmaplist_program} | grep -qx 'lengths: 1,2,3' && ${e2e_workdir}/interp_maplist_app ${e2e_interpmaplist_program} | grep -qx 'sum sq: 30'")
set_tests_properties(e2e_interp_maplist_demo_verify PROPERTIES DEPENDS e2e_interp_maplist_demo_compile)

# filterList(xs, fn): the higher-order companion to mapList. It keeps each element
# for which the named predicate is truthy (resolved through callFunc, decided by
# the same truthy rule as if / while). The committed program filters by user
# predicates, folds the result with sum / len, composes filter then map then join,
# and covers the empty-filter case.
set(e2e_interpfilter_input   ${CMAKE_SOURCE_DIR}/examples/interp_filter_demo.vst)
set(e2e_interpfilter_program ${CMAKE_SOURCE_DIR}/tests/e2e/interp_filter_program.calc)

add_test(
    NAME    e2e_interp_filter_demo_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_interpfilter_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_interp_filter_demo_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_workdir}/interp_filter_demo.cpp
            -o ${e2e_workdir}/interp_filter_app)
set_tests_properties(e2e_interp_filter_demo_compile PROPERTIES DEPENDS e2e_interp_filter_demo_transpile)

# Run the filter interpreter on the committed program; final `result: done`.
add_test(
    NAME    e2e_interp_filter_demo_run
    COMMAND ${e2e_workdir}/interp_filter_app ${e2e_interpfilter_program})
set_tests_properties(e2e_interp_filter_demo_run PROPERTIES
    DEPENDS                 e2e_interp_filter_demo_compile
    PASS_REGULAR_EXPRESSION "result: done")

# Verify filtering reached stdout: a predicate-filtered list (evens: 2,4,6,8,10)
# and a filter-then-map-then-join pipeline (even sq: 4 16 36 64 100).
add_test(
    NAME    e2e_interp_filter_demo_verify
    COMMAND sh -c "${e2e_workdir}/interp_filter_app ${e2e_interpfilter_program} | grep -qx 'evens: 2,4,6,8,10' && ${e2e_workdir}/interp_filter_app ${e2e_interpfilter_program} | grep -qx 'even sq: 4 16 36 64 100' && ${e2e_workdir}/interp_filter_app ${e2e_interpfilter_program} | grep -qx 'sum evens: 30'")
set_tests_properties(e2e_interp_filter_demo_verify PROPERTIES DEPENDS e2e_interp_filter_demo_compile)

# reduceList(xs, fn, init): a left fold with a two-argument user function, the
# general form of sum / min / max and the first builtin to drive a binary user
# function (resolved through callFunc). The committed program expresses sum,
# product, max, count, and a string concatenation as folds, plus the empty-list
# case returning init.
set(e2e_interpfold_input   ${CMAKE_SOURCE_DIR}/examples/interp_fold_demo.vst)
set(e2e_interpfold_program ${CMAKE_SOURCE_DIR}/tests/e2e/interp_fold_program.calc)

add_test(
    NAME    e2e_interp_fold_demo_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_interpfold_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_interp_fold_demo_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_workdir}/interp_fold_demo.cpp
            -o ${e2e_workdir}/interp_fold_app)
set_tests_properties(e2e_interp_fold_demo_compile PROPERTIES DEPENDS e2e_interp_fold_demo_transpile)

# Run the fold interpreter on the committed program; final `result: done`.
add_test(
    NAME    e2e_interp_fold_demo_run
    COMMAND ${e2e_workdir}/interp_fold_app ${e2e_interpfold_program})
set_tests_properties(e2e_interp_fold_demo_run PROPERTIES
    DEPENDS                 e2e_interp_fold_demo_compile
    PASS_REGULAR_EXPRESSION "result: done")

# Verify folds reached stdout: a sum fold (sum: 14), a product fold (product: 60),
# and a string-concatenation fold (concat: abc).
add_test(
    NAME    e2e_interp_fold_demo_verify
    COMMAND sh -c "${e2e_workdir}/interp_fold_app ${e2e_interpfold_program} | grep -qx 'sum: 14' && ${e2e_workdir}/interp_fold_app ${e2e_interpfold_program} | grep -qx 'product: 60' && ${e2e_workdir}/interp_fold_app ${e2e_interpfold_program} | grep -qx 'concat: abc'")
set_tests_properties(e2e_interp_fold_demo_verify PROPERTIES DEPENDS e2e_interp_fold_demo_compile)

# range(n): a list producer for [0, n), the generator that completes the
# comprehension shape (produce a range, then map / filter / reduce over it). The
# committed program feeds range into the higher-order trio: range -> map, range ->
# filter, range -> reduce, range -> map -> sum, plus the empty and negative cases.
set(e2e_interprange_input   ${CMAKE_SOURCE_DIR}/examples/interp_range_demo.vst)
set(e2e_interprange_program ${CMAKE_SOURCE_DIR}/tests/e2e/interp_range_program.calc)

add_test(
    NAME    e2e_interp_range_demo_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_interprange_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_interp_range_demo_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_workdir}/interp_range_demo.cpp
            -o ${e2e_workdir}/interp_range_app)
set_tests_properties(e2e_interp_range_demo_compile PROPERTIES DEPENDS e2e_interp_range_demo_transpile)

# Run the range interpreter on the committed program; final `result: done`.
add_test(
    NAME    e2e_interp_range_demo_run
    COMMAND ${e2e_workdir}/interp_range_app ${e2e_interprange_program})
set_tests_properties(e2e_interp_range_demo_run PROPERTIES
    DEPENDS                 e2e_interp_range_demo_compile
    PASS_REGULAR_EXPRESSION "result: done")

# Verify the comprehension pipeline reached stdout: range -> map (squares:
# 0,1,4,9,16) and range -> reduce (sum 0..99: 4950).
add_test(
    NAME    e2e_interp_range_demo_verify
    COMMAND sh -c "${e2e_workdir}/interp_range_app ${e2e_interprange_program} | grep -qx 'squares: 0,1,4,9,16' && ${e2e_workdir}/interp_range_app ${e2e_interprange_program} | grep -qx 'sum 0..99: 4950' && ${e2e_workdir}/interp_range_app ${e2e_interprange_program} | grep -qx 'odds: 1,3,5,7,9'")
set_tests_properties(e2e_interp_range_demo_verify PROPERTIES DEPENDS e2e_interp_range_demo_compile)

# zipMap(keys, values): build a map from two parallel lists, the bridge from the
# list type into the map type. Each key element is rendered with format (so integer
# keys from range become string keys), pairing up to the shorter list. The committed
# program zips parallel lists, zips range with mapped squares, and handles a length
# mismatch.
set(e2e_interpzip_input   ${CMAKE_SOURCE_DIR}/examples/interp_zipmap_demo.vst)
set(e2e_interpzip_program ${CMAKE_SOURCE_DIR}/tests/e2e/interp_zipmap_program.calc)

add_test(
    NAME    e2e_interp_zipmap_demo_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_interpzip_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_interp_zipmap_demo_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_workdir}/interp_zipmap_demo.cpp
            -o ${e2e_workdir}/interp_zipmap_app)
set_tests_properties(e2e_interp_zipmap_demo_compile PROPERTIES DEPENDS e2e_interp_zipmap_demo_transpile)

# Run the zipMap interpreter on the committed program; final `result: done`.
add_test(
    NAME    e2e_interp_zipmap_demo_run
    COMMAND ${e2e_workdir}/interp_zipmap_app ${e2e_interpzip_program})
set_tests_properties(e2e_interp_zipmap_demo_run PROPERTIES
    DEPENDS                 e2e_interp_zipmap_demo_compile
    PASS_REGULAR_EXPRESSION "result: done")

# Verify zipMap reached stdout: a value looked up by zipped key (b: 2) and a
# number-to-square table built from range + mapList (sq3: 9).
add_test(
    NAME    e2e_interp_zipmap_demo_verify
    COMMAND sh -c "${e2e_workdir}/interp_zipmap_app ${e2e_interpzip_program} | grep -qx 'b: 2' && ${e2e_workdir}/interp_zipmap_app ${e2e_interpzip_program} | grep -qx 'sq3: 9' && ${e2e_workdir}/interp_zipmap_app ${e2e_interpzip_program} | grep -qx 'sorted: a,b,c'")
set_tests_properties(e2e_interp_zipmap_demo_verify PROPERTIES DEPENDS e2e_interp_zipmap_demo_compile)

# values(m) / entries(m): read a map's contents as lists, completing map iteration
# alongside keys(m). values returns the value list; entries returns one [key, value]
# two-element list per binding (read with at, since there is no tuple type). The
# committed program aggregates over values (sum / max / sort) and sums values by
# walking entries with nested at.
set(e2e_interpvalues_input   ${CMAKE_SOURCE_DIR}/examples/interp_values_demo.vst)
set(e2e_interpvalues_program ${CMAKE_SOURCE_DIR}/tests/e2e/interp_values_program.calc)

add_test(
    NAME    e2e_interp_values_demo_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_interpvalues_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_interp_values_demo_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_workdir}/interp_values_demo.cpp
            -o ${e2e_workdir}/interp_values_app)
set_tests_properties(e2e_interp_values_demo_compile PROPERTIES DEPENDS e2e_interp_values_demo_transpile)

# Run the values interpreter on the committed program; final `result: done`.
add_test(
    NAME    e2e_interp_values_demo_run
    COMMAND ${e2e_workdir}/interp_values_app ${e2e_interpvalues_program})
set_tests_properties(e2e_interp_values_demo_run PROPERTIES
    DEPENDS                 e2e_interp_values_demo_compile
    PASS_REGULAR_EXPRESSION "result: done")

# Verify map-reading reached stdout: a sum over the value list (vsum: 6) and a
# sum walked over entries with nested at (entry sum: 6).
add_test(
    NAME    e2e_interp_values_demo_verify
    COMMAND sh -c "${e2e_workdir}/interp_values_app ${e2e_interpvalues_program} | grep -qx 'vsum: 6' && ${e2e_workdir}/interp_values_app ${e2e_interpvalues_program} | grep -qx 'entry sum: 6' && ${e2e_workdir}/interp_values_app ${e2e_interpvalues_program} | grep -qx 'sorted vals: 1,2,3'")
set_tests_properties(e2e_interp_values_demo_verify PROPERTIES DEPENDS e2e_interp_values_demo_compile)

# set(xs, i, v) / remove(m, k): functional modification by index / key. set copies
# the list and overwrites one slot via Vec.set; remove rebuilds the map without one
# key (HashMap has no in-place delete). Both leave their inputs intact. The
# committed program replaces a list element, removes a map key, and checks the
# originals are unchanged plus the out-of-range / missing-key no-ops.
set(e2e_interpsetrm_input   ${CMAKE_SOURCE_DIR}/examples/interp_setremove_demo.vst)
set(e2e_interpsetrm_program ${CMAKE_SOURCE_DIR}/tests/e2e/interp_setremove_program.calc)

add_test(
    NAME    e2e_interp_setremove_demo_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_interpsetrm_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_interp_setremove_demo_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_workdir}/interp_setremove_demo.cpp
            -o ${e2e_workdir}/interp_setremove_app)
set_tests_properties(e2e_interp_setremove_demo_compile PROPERTIES DEPENDS e2e_interp_setremove_demo_transpile)

# Run the set/remove interpreter on the committed program; final `result: done`.
add_test(
    NAME    e2e_interp_setremove_demo_run
    COMMAND ${e2e_workdir}/interp_setremove_app ${e2e_interpsetrm_program})
set_tests_properties(e2e_interp_setremove_demo_run PROPERTIES
    DEPENDS                 e2e_interp_setremove_demo_compile
    PASS_REGULAR_EXPRESSION "result: done")

# Verify functional modification reached stdout: a list element replaced
# (set: 10,99,30) and a map key removed (removed b: a,c), with the original intact.
add_test(
    NAME    e2e_interp_setremove_demo_verify
    COMMAND sh -c "${e2e_workdir}/interp_setremove_app ${e2e_interpsetrm_program} | grep -qx 'set: 10,99,30' && ${e2e_workdir}/interp_setremove_app ${e2e_interpsetrm_program} | grep -qx 'removed b: a,c' && ${e2e_workdir}/interp_setremove_app ${e2e_interpsetrm_program} | grep -qx 'orig: 10,20,30'")
set_tests_properties(e2e_interp_setremove_demo_verify PROPERTIES DEPENDS e2e_interp_setremove_demo_compile)

# Numeric builtins abs / mod: round out the arithmetic surface (the operators give
# no modulo). mod(a, b) = a - (a / b) * b (0-guarded), abs(x) the absolute value;
# both coerce operands with asInt. With mod an even check is mod(x, 2) == 0, and the
# higher-order machinery makes FizzBuzz a one-liner. The committed program covers
# abs, mod, the divide-by-zero guard, a mod-based even filter, and a FizzBuzz map.
set(e2e_interparith_input   ${CMAKE_SOURCE_DIR}/examples/interp_arith_demo.vst)
set(e2e_interparith_program ${CMAKE_SOURCE_DIR}/tests/e2e/interp_arith_program.calc)

add_test(
    NAME    e2e_interp_arith_demo_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_interparith_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_interp_arith_demo_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_workdir}/interp_arith_demo.cpp
            -o ${e2e_workdir}/interp_arith_app)
set_tests_properties(e2e_interp_arith_demo_compile PROPERTIES DEPENDS e2e_interp_arith_demo_transpile)

# Run the arith interpreter on the committed program; final `result: done`.
add_test(
    NAME    e2e_interp_arith_demo_run
    COMMAND ${e2e_workdir}/interp_arith_app ${e2e_interparith_program})
set_tests_properties(e2e_interp_arith_demo_run PROPERTIES
    DEPENDS                 e2e_interp_arith_demo_compile
    PASS_REGULAR_EXPRESSION "result: done")

# Verify the arithmetic reached stdout: a modulo (mod: 2) and the FizzBuzz map.
add_test(
    NAME    e2e_interp_arith_demo_verify
    COMMAND sh -c "${e2e_workdir}/interp_arith_app ${e2e_interparith_program} | grep -qx 'mod: 2' && ${e2e_workdir}/interp_arith_app ${e2e_interparith_program} | grep -qx 'fizzbuzz: 1 2 Fizz 4 Buzz Fizz 7 8 Fizz Buzz 11 Fizz 13 14 FizzBuzz' && ${e2e_workdir}/interp_arith_app ${e2e_interparith_program} | grep -qx 'abs neg: 7'")
set_tests_properties(e2e_interp_arith_demo_verify PROPERTIES DEPENDS e2e_interp_arith_demo_compile)

# Unary minus: a leading `-` on a factor, closing the gap the abs slice found
# (negatives previously had to be written as 0 - n). Handled in pFactor, so it binds
# tighter than the binary operators and composes everywhere a factor can. The
# committed program covers a negative literal, precedence vs *, double negation,
# negating a parenthesized expression, a negative variable, and -n in a function.
set(e2e_interpneg_input   ${CMAKE_SOURCE_DIR}/examples/interp_unaryminus_demo.vst)
set(e2e_interpneg_program ${CMAKE_SOURCE_DIR}/tests/e2e/interp_unaryminus_program.calc)

add_test(
    NAME    e2e_interp_unaryminus_demo_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_interpneg_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_interp_unaryminus_demo_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_workdir}/interp_unaryminus_demo.cpp
            -o ${e2e_workdir}/interp_unaryminus_app)
set_tests_properties(e2e_interp_unaryminus_demo_compile PROPERTIES DEPENDS e2e_interp_unaryminus_demo_transpile)

# Run the unary-minus interpreter on the committed program; final `result: done`.
add_test(
    NAME    e2e_interp_unaryminus_demo_run
    COMMAND ${e2e_workdir}/interp_unaryminus_app ${e2e_interpneg_program})
set_tests_properties(e2e_interp_unaryminus_demo_run PROPERTIES
    DEPENDS                 e2e_interp_unaryminus_demo_compile
    PASS_REGULAR_EXPRESSION "result: done")

# Verify unary minus reached stdout: precedence vs * (neg mul: -12) and negating a
# parenthesized expression (neg paren: -5).
add_test(
    NAME    e2e_interp_unaryminus_demo_verify
    COMMAND sh -c "${e2e_workdir}/interp_unaryminus_app ${e2e_interpneg_program} | grep -qx 'neg mul: -12' && ${e2e_workdir}/interp_unaryminus_app ${e2e_interpneg_program} | grep -qx 'neg paren: -5' && ${e2e_workdir}/interp_unaryminus_app ${e2e_interpneg_program} | grep -qx 'abs lit: 9'")
set_tests_properties(e2e_interp_unaryminus_demo_verify PROPERTIES DEPENDS e2e_interp_unaryminus_demo_compile)

# Membership tests member / hasValue: search the compound types by value (not just
# by key / string content). Both compare with the structural valEq, so the test is
# type-aware (int 20 matches int 20 but not string "20"). The committed program
# tests list membership, map value membership, the cross-type case, and empties.
set(e2e_interpmember_input   ${CMAKE_SOURCE_DIR}/examples/interp_member_demo.vst)
set(e2e_interpmember_program ${CMAKE_SOURCE_DIR}/tests/e2e/interp_member_program.calc)

add_test(
    NAME    e2e_interp_member_demo_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_interpmember_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_interp_member_demo_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_workdir}/interp_member_demo.cpp
            -o ${e2e_workdir}/interp_member_app)
set_tests_properties(e2e_interp_member_demo_compile PROPERTIES DEPENDS e2e_interp_member_demo_transpile)

# Run the member interpreter on the committed program; final `result: done`.
add_test(
    NAME    e2e_interp_member_demo_run
    COMMAND ${e2e_workdir}/interp_member_app ${e2e_interpmember_program})
set_tests_properties(e2e_interp_member_demo_run PROPERTIES
    DEPENDS                 e2e_interp_member_demo_compile
    PASS_REGULAR_EXPRESSION "result: done")

# Verify membership reached stdout: a list hit (has banana: true), a map value hit
# (hasval 2: true), and the cross-type miss (cross type: false).
add_test(
    NAME    e2e_interp_member_demo_verify
    COMMAND sh -c "${e2e_workdir}/interp_member_app ${e2e_interpmember_program} | grep -qx 'has banana: true' && ${e2e_workdir}/interp_member_app ${e2e_interpmember_program} | grep -qx 'hasval 2: true' && ${e2e_workdir}/interp_member_app ${e2e_interpmember_program} | grep -qx 'cross type: false'")
set_tests_properties(e2e_interp_member_demo_verify PROPERTIES DEPENDS e2e_interp_member_demo_compile)

# List slicing take / drop: take(xs, n) is the first n elements, drop(xs, n) the
# rest; both functional and count-clamped. They partition a list (concat(take,
# drop) == xs) and compose into pagination (take(drop(xs, k), n)). The committed
# program covers prefix / suffix, the clamps, the partition identity, a pagination
# slice, and a bounded prefix of a computed list.
set(e2e_interptd_input   ${CMAKE_SOURCE_DIR}/examples/interp_takedrop_demo.vst)
set(e2e_interptd_program ${CMAKE_SOURCE_DIR}/tests/e2e/interp_takedrop_program.calc)

add_test(
    NAME    e2e_interp_takedrop_demo_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_interptd_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_interp_takedrop_demo_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_workdir}/interp_takedrop_demo.cpp
            -o ${e2e_workdir}/interp_takedrop_app)
set_tests_properties(e2e_interp_takedrop_demo_compile PROPERTIES DEPENDS e2e_interp_takedrop_demo_transpile)

# Run the take/drop interpreter on the committed program; final `result: done`.
add_test(
    NAME    e2e_interp_takedrop_demo_run
    COMMAND ${e2e_workdir}/interp_takedrop_app ${e2e_interptd_program})
set_tests_properties(e2e_interp_takedrop_demo_run PROPERTIES
    DEPENDS                 e2e_interp_takedrop_demo_compile
    PASS_REGULAR_EXPRESSION "result: done")

# Verify slicing reached stdout: a prefix (take 2: a,b), a pagination slice
# (page2: 3,4,5), and the partition identity (partition: a,b,c,d,e).
add_test(
    NAME    e2e_interp_takedrop_demo_verify
    COMMAND sh -c "${e2e_workdir}/interp_takedrop_app ${e2e_interptd_program} | grep -qx 'take 2: a,b' && ${e2e_workdir}/interp_takedrop_app ${e2e_interptd_program} | grep -qx 'page2: 3,4,5' && ${e2e_workdir}/interp_takedrop_app ${e2e_interptd_program} | grep -qx 'partition: a,b,c,d,e'")
set_tests_properties(e2e_interp_takedrop_demo_verify PROPERTIES DEPENDS e2e_interp_takedrop_demo_compile)

# reverse(xs) / indexOf(xs, v): reverse is an involution built by walking the Vec
# from the end; indexOf is the position counterpart to member, using the structural
# valEq (type-aware), returning the first matching index or -1. The committed
# program reverses lists (including range), checks the double-reverse identity, and
# finds / fails to find values by index.
set(e2e_interpri_input   ${CMAKE_SOURCE_DIR}/examples/interp_revidx_demo.vst)
set(e2e_interpri_program ${CMAKE_SOURCE_DIR}/tests/e2e/interp_revidx_program.calc)

add_test(
    NAME    e2e_interp_revidx_demo_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_interpri_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_interp_revidx_demo_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_workdir}/interp_revidx_demo.cpp
            -o ${e2e_workdir}/interp_revidx_app)
set_tests_properties(e2e_interp_revidx_demo_compile PROPERTIES DEPENDS e2e_interp_revidx_demo_transpile)

# Run the reverse/indexOf interpreter on the committed program; final `result: done`.
add_test(
    NAME    e2e_interp_revidx_demo_run
    COMMAND ${e2e_workdir}/interp_revidx_app ${e2e_interpri_program})
set_tests_properties(e2e_interp_revidx_demo_run PROPERTIES
    DEPENDS                 e2e_interp_revidx_demo_compile
    PASS_REGULAR_EXPRESSION "result: done")

# Verify reverse / indexOf reached stdout: a reversed list (reversed: d,c,b,a) and
# a found index (idx c: 2).
add_test(
    NAME    e2e_interp_revidx_demo_verify
    COMMAND sh -c "${e2e_workdir}/interp_revidx_app ${e2e_interpri_program} | grep -qx 'reversed: d,c,b,a' && ${e2e_workdir}/interp_revidx_app ${e2e_interpri_program} | grep -qx 'idx c: 2' && ${e2e_workdir}/interp_revidx_app ${e2e_interpri_program} | grep -qx 'rev range: 4,3,2,1,0'")
set_tests_properties(e2e_interp_revidx_demo_verify PROPERTIES DEPENDS e2e_interp_revidx_demo_compile)

# zip(xs, ys): pair two lists element-wise into a list of two-element lists (no
# tuple type), the list counterpart of zipMap, pairing up to the shorter input. The
# committed program zips parallel lists, reads pairs with nested at, builds name=age
# strings, zips a range for indices, and handles a length mismatch.
set(e2e_interpzl_input   ${CMAKE_SOURCE_DIR}/examples/interp_zip_demo.vst)
set(e2e_interpzl_program ${CMAKE_SOURCE_DIR}/tests/e2e/interp_zip_program.calc)

add_test(
    NAME    e2e_interp_zip_demo_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_interpzl_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_interp_zip_demo_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_workdir}/interp_zip_demo.cpp
            -o ${e2e_workdir}/interp_zip_app)
set_tests_properties(e2e_interp_zip_demo_compile PROPERTIES DEPENDS e2e_interp_zip_demo_transpile)

# Run the zip interpreter on the committed program; final `result: done`.
add_test(
    NAME    e2e_interp_zip_demo_run
    COMMAND ${e2e_workdir}/interp_zip_app ${e2e_interpzl_program})
set_tests_properties(e2e_interp_zip_demo_run PROPERTIES
    DEPENDS                 e2e_interp_zip_demo_compile
    PASS_REGULAR_EXPRESSION "result: done")

# Verify zip reached stdout: a paired-and-rendered list (joined:
# alice=30,bob=25,carol=35) and a range-indexed pair (indexed: 1 bob).
add_test(
    NAME    e2e_interp_zip_demo_verify
    COMMAND sh -c "${e2e_workdir}/interp_zip_app ${e2e_interpzl_program} | grep -qx 'joined: alice=30,bob=25,carol=35' && ${e2e_workdir}/interp_zip_app ${e2e_interpzl_program} | grep -qx 'indexed: 1 bob'")
set_tests_properties(e2e_interp_zip_demo_verify PROPERTIES DEPENDS e2e_interp_zip_demo_compile)

# groupBy(xs, fn): bucket a list into a map keyed by fn(element), the interpreter's
# richest builtin (higher-order callFunc + map + functional list buckets). Buckets
# preserve input order. The committed program partitions numbers by parity and
# words by first letter, counts groups, and folds a bucket with sum.
set(e2e_interpgb_input   ${CMAKE_SOURCE_DIR}/examples/interp_groupby_demo.vst)
set(e2e_interpgb_program ${CMAKE_SOURCE_DIR}/tests/e2e/interp_groupby_program.calc)

add_test(
    NAME    e2e_interp_groupby_demo_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_interpgb_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_interp_groupby_demo_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_workdir}/interp_groupby_demo.cpp
            -o ${e2e_workdir}/interp_groupby_app)
set_tests_properties(e2e_interp_groupby_demo_compile PROPERTIES DEPENDS e2e_interp_groupby_demo_transpile)

# Run the groupBy interpreter on the committed program; final `result: done`.
add_test(
    NAME    e2e_interp_groupby_demo_run
    COMMAND ${e2e_workdir}/interp_groupby_app ${e2e_interpgb_program})
set_tests_properties(e2e_interp_groupby_demo_run PROPERTIES
    DEPENDS                 e2e_interp_groupby_demo_compile
    PASS_REGULAR_EXPRESSION "result: done")

# Verify grouping reached stdout: a parity bucket (evens: 0,2,4,6,8) and a
# first-letter bucket (a words: apple,avocado).
add_test(
    NAME    e2e_interp_groupby_demo_verify
    COMMAND sh -c "${e2e_workdir}/interp_groupby_app ${e2e_interpgb_program} | grep -qx 'evens: 0,2,4,6,8' && ${e2e_workdir}/interp_groupby_app ${e2e_interpgb_program} | grep -qx 'a words: apple,avocado' && ${e2e_workdir}/interp_groupby_app ${e2e_interpgb_program} | grep -qx 'sum evens: 20'")
set_tests_properties(e2e_interp_groupby_demo_verify PROPERTIES DEPENDS e2e_interp_groupby_demo_compile)

# update(m, k, fn): apply a one-arg function to a single map value (int(0) if the
# key is absent), the derive-from-old counterpart to put. Resolved through callFunc.
# A missing key reading as 0 makes the word-frequency count just update(counts, w,
# "inc"). The committed program counts words, updates with another fn, and the
# missing-key case.
set(e2e_interpup_input   ${CMAKE_SOURCE_DIR}/examples/interp_update_demo.vst)
set(e2e_interpup_program ${CMAKE_SOURCE_DIR}/tests/e2e/interp_update_program.calc)

add_test(
    NAME    e2e_interp_update_demo_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_interpup_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_interp_update_demo_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_workdir}/interp_update_demo.cpp
            -o ${e2e_workdir}/interp_update_app)
set_tests_properties(e2e_interp_update_demo_compile PROPERTIES DEPENDS e2e_interp_update_demo_transpile)

# Run the update interpreter on the committed program; final `result: done`.
add_test(
    NAME    e2e_interp_update_demo_run
    COMMAND ${e2e_workdir}/interp_update_app ${e2e_interpup_program})
set_tests_properties(e2e_interp_update_demo_run PROPERTIES
    DEPENDS                 e2e_interp_update_demo_compile
    PASS_REGULAR_EXPRESSION "result: done")

# Verify update reached stdout: a word-frequency count (the: 3) and a derive-from-old
# update (the doubled: 6).
add_test(
    NAME    e2e_interp_update_demo_verify
    COMMAND sh -c "${e2e_workdir}/interp_update_app ${e2e_interpup_program} | grep -qx 'the: 3' && ${e2e_workdir}/interp_update_app ${e2e_interpup_program} | grep -qx 'the doubled: 6' && ${e2e_workdir}/interp_update_app ${e2e_interpup_program} | grep -qx 'new: 1'")
set_tests_properties(e2e_interp_update_demo_verify PROPERTIES DEPENDS e2e_interp_update_demo_compile)

# merge(m1, m2): combine two maps, m1 overlaid with m2 (m2 wins on a shared key).
# Functional (a copy of m1 with m2's entries set on top), so both inputs are intact.
# Completes the map combiners alongside put and update. The committed program merges
# overlapping maps, checks the override and the untouched original, and an empty merge.
set(e2e_interpmg_input   ${CMAKE_SOURCE_DIR}/examples/interp_merge_demo.vst)
set(e2e_interpmg_program ${CMAKE_SOURCE_DIR}/tests/e2e/interp_merge_program.calc)

add_test(
    NAME    e2e_interp_merge_demo_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_interpmg_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_interp_merge_demo_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_workdir}/interp_merge_demo.cpp
            -o ${e2e_workdir}/interp_merge_app)
set_tests_properties(e2e_interp_merge_demo_compile PROPERTIES DEPENDS e2e_interp_merge_demo_transpile)

# Run the merge interpreter on the committed program; final `result: done`.
add_test(
    NAME    e2e_interp_merge_demo_run
    COMMAND ${e2e_workdir}/interp_merge_app ${e2e_interpmg_program})
set_tests_properties(e2e_interp_merge_demo_run PROPERTIES
    DEPENDS                 e2e_interp_merge_demo_compile
    PASS_REGULAR_EXPRESSION "result: done")

# Verify merge reached stdout: the override (b: 20) and the merged key set
# (sorted: a,b,c).
add_test(
    NAME    e2e_interp_merge_demo_verify
    COMMAND sh -c "${e2e_workdir}/interp_merge_app ${e2e_interpmg_program} | grep -qx 'b: 20' && ${e2e_workdir}/interp_merge_app ${e2e_interpmg_program} | grep -qx 'sorted: a,b,c' && ${e2e_workdir}/interp_merge_app ${e2e_interpmg_program} | grep -qx 'total: 51'")
set_tests_properties(e2e_interp_merge_demo_verify PROPERTIES DEPENDS e2e_interp_merge_demo_compile)

# any(xs, fn) / all(xs, fn): higher-order boolean quantifiers over a predicate
# (resolved through callFunc, judged by truthy, short-circuiting). any of nothing is
# false, all of nothing is true. The committed program tests all-true / mixed lists,
# and the empty-list vacuous cases.
set(e2e_interpaa_input   ${CMAKE_SOURCE_DIR}/examples/interp_anyall_demo.vst)
set(e2e_interpaa_program ${CMAKE_SOURCE_DIR}/tests/e2e/interp_anyall_program.calc)

add_test(
    NAME    e2e_interp_anyall_demo_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_interpaa_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_interp_anyall_demo_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_workdir}/interp_anyall_demo.cpp
            -o ${e2e_workdir}/interp_anyall_app)
set_tests_properties(e2e_interp_anyall_demo_compile PROPERTIES DEPENDS e2e_interp_anyall_demo_transpile)

# Run the any/all interpreter on the committed program; final `result: done`.
add_test(
    NAME    e2e_interp_anyall_demo_run
    COMMAND ${e2e_workdir}/interp_anyall_app ${e2e_interpaa_program})
set_tests_properties(e2e_interp_anyall_demo_run PROPERTIES
    DEPENDS                 e2e_interp_anyall_demo_compile
    PASS_REGULAR_EXPRESSION "result: done")

# Verify quantifiers reached stdout: an all-true (all even: true), a mixed any
# (any even mixed: true), and the vacuous all (all empty: true).
add_test(
    NAME    e2e_interp_anyall_demo_verify
    COMMAND sh -c "${e2e_workdir}/interp_anyall_app ${e2e_interpaa_program} | grep -qx 'all even: true' && ${e2e_workdir}/interp_anyall_app ${e2e_interpaa_program} | grep -qx 'any even mixed: true' && ${e2e_workdir}/interp_anyall_app ${e2e_interpaa_program} | grep -qx 'all empty: true'")
set_tests_properties(e2e_interp_anyall_demo_verify PROPERTIES DEPENDS e2e_interp_anyall_demo_compile)

# replace(s, old, new): replace every occurrence of a substring, implemented over
# Str.split (append each piece with new between), which handles the edges (adjacent
# matches, empty old) for free. The committed program covers single- and multi-char
# replaces, removal, a no-match no-op, adjacent matches, and a round-trip.
set(e2e_interprp_input   ${CMAKE_SOURCE_DIR}/examples/interp_replace_demo.vst)
set(e2e_interprp_program ${CMAKE_SOURCE_DIR}/tests/e2e/interp_replace_program.calc)

add_test(
    NAME    e2e_interp_replace_demo_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_interprp_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_interp_replace_demo_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_workdir}/interp_replace_demo.cpp
            -o ${e2e_workdir}/interp_replace_app)
set_tests_properties(e2e_interp_replace_demo_compile PROPERTIES DEPENDS e2e_interp_replace_demo_transpile)

# Run the replace interpreter on the committed program; final `result: done`.
add_test(
    NAME    e2e_interp_replace_demo_run
    COMMAND ${e2e_workdir}/interp_replace_app ${e2e_interprp_program})
set_tests_properties(e2e_interp_replace_demo_run PROPERTIES
    DEPENDS                 e2e_interp_replace_demo_compile
    PASS_REGULAR_EXPRESSION "result: done")

# Verify replace reached stdout: a delimiter swap (slashes: a/b/c) and a word
# replace (word: the dog sat).
add_test(
    NAME    e2e_interp_replace_demo_verify
    COMMAND sh -c "${e2e_workdir}/interp_replace_app ${e2e_interprp_program} | grep -qx 'slashes: a/b/c' && ${e2e_workdir}/interp_replace_app ${e2e_interprp_program} | grep -qx 'word: the dog sat' && ${e2e_workdir}/interp_replace_app ${e2e_interprp_program} | grep -qx 'adjacent: xxx'")
set_tests_properties(e2e_interp_replace_demo_verify PROPERTIES DEPENDS e2e_interp_replace_demo_compile)

# Capstone: the complete interpreter (interp_replace_demo.vst, the full builtin set)
# running a real word-frequency report program, end to end. No new interpreter code;
# this proves the whole language solves a genuine task (tokenize, count, sort,
# aggregate, render) rather than exercising one feature. Reuses the latest complete
# interpreter input.
set(e2e_interprep_input   ${CMAKE_SOURCE_DIR}/examples/interp_replace_demo.vst)
set(e2e_interprep_program ${CMAKE_SOURCE_DIR}/tests/e2e/interp_report_program.calc)

add_test(
    NAME    e2e_interp_report_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_interprep_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_interp_report_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_workdir}/interp_replace_demo.cpp
            -o ${e2e_workdir}/interp_report_app)
set_tests_properties(e2e_interp_report_compile PROPERTIES DEPENDS e2e_interp_report_transpile)

# Run the complete interpreter on the committed report program; final `report done`.
add_test(
    NAME    e2e_interp_report_run
    COMMAND ${e2e_workdir}/interp_report_app ${e2e_interprep_program})
set_tests_properties(e2e_interp_report_run PROPERTIES
    DEPENDS                 e2e_interp_report_compile
    PASS_REGULAR_EXPRESSION "result: report done")

# Verify the report's analytics reached stdout: the word total, the length-reduce
# (longest word: quick), and the replace-normalized sorted key list.
add_test(
    NAME    e2e_interp_report_verify
    COMMAND sh -c "${e2e_workdir}/interp_report_app ${e2e_interprep_program} | grep -qx 'total words: 10' && ${e2e_workdir}/interp_report_app ${e2e_interprep_program} | grep -qx 'longest word: quick' && ${e2e_workdir}/interp_report_app ${e2e_interprep_program} | grep -qx 'normalized: brown_dog_fox_lazy_quick_the'")
set_tests_properties(e2e_interp_report_verify PROPERTIES DEPENDS e2e_interp_report_compile)

# Comparison operators <= / >= / !=: close the gap the capstone found (the language
# had only < / > / ==). Three new tokens lexed before the single-char fallback, plus
# pCmp arms for <= / >= (asInt compare) and a pEq arm for != (!valEq). The committed
# program tests each operator including cross-type !=, uses them in user predicates
# for filterList / reduceList, and checks the existing < / == still work.
set(e2e_interpco_input   ${CMAKE_SOURCE_DIR}/examples/interp_cmpops_demo.vst)
set(e2e_interpco_program ${CMAKE_SOURCE_DIR}/tests/e2e/interp_cmpops_program.calc)

add_test(
    NAME    e2e_interp_cmpops_demo_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_interpco_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_interp_cmpops_demo_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_workdir}/interp_cmpops_demo.cpp
            -o ${e2e_workdir}/interp_cmpops_app)
set_tests_properties(e2e_interp_cmpops_demo_compile PROPERTIES DEPENDS e2e_interp_cmpops_demo_transpile)

# Run the cmpops interpreter on the committed program; final `result: done`.
add_test(
    NAME    e2e_interp_cmpops_demo_run
    COMMAND ${e2e_workdir}/interp_cmpops_app ${e2e_interpco_program})
set_tests_properties(e2e_interp_cmpops_demo_run PROPERTIES
    DEPENDS                 e2e_interp_cmpops_demo_compile
    PASS_REGULAR_EXPRESSION "result: done")

# Verify the operators reached stdout: an equal <= (le eq: true), a cross-type !=
# (ne crosstype: true), and a >= predicate filter (filtered: 5,6,7).
add_test(
    NAME    e2e_interp_cmpops_demo_verify
    COMMAND sh -c "${e2e_workdir}/interp_cmpops_app ${e2e_interpco_program} | grep -qx 'le eq: true' && ${e2e_workdir}/interp_cmpops_app ${e2e_interpco_program} | grep -qx 'ne crosstype: true' && ${e2e_workdir}/interp_cmpops_app ${e2e_interpco_program} | grep -qx 'filtered: 5,6,7'")
set_tests_properties(e2e_interp_cmpops_demo_verify PROPERTIES DEPENDS e2e_interp_cmpops_demo_compile)

# Logical and / or with short-circuit: two new precedence levels (pOr over pAnd over
# pEq) below the comparisons. They short-circuit via the live flag (the right operand
# is parsed to advance the cursor but only evaluated when the left does not decide
# the result), so `true or boom()` never calls boom. The committed program checks the
# truth tables, precedence (and binds tighter than or), short-circuit (an
# infinitely-recursive fn on the skipped side), and use in a user predicate.
set(e2e_interplc_input   ${CMAKE_SOURCE_DIR}/examples/interp_logic_demo.vst)
set(e2e_interplc_program ${CMAKE_SOURCE_DIR}/tests/e2e/interp_logic_program.calc)

add_test(
    NAME    e2e_interp_logic_demo_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_interplc_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_interp_logic_demo_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_workdir}/interp_logic_demo.cpp
            -o ${e2e_workdir}/interp_logic_app)
set_tests_properties(e2e_interp_logic_demo_compile PROPERTIES DEPENDS e2e_interp_logic_demo_transpile)

# Run the logic interpreter on the committed program; final `result: done`.
add_test(
    NAME    e2e_interp_logic_demo_run
    COMMAND ${e2e_workdir}/interp_logic_app ${e2e_interplc_program})
set_tests_properties(e2e_interp_logic_demo_run PROPERTIES
    DEPENDS                 e2e_interp_logic_demo_compile
    PASS_REGULAR_EXPRESSION "result: done")

# Verify and / or reached stdout: a false conjunction (and tf: false), the
# short-circuit (short or: true, boom never called), and a compound user predicate
# filter (filtered: 0,2,4).
add_test(
    NAME    e2e_interp_logic_demo_verify
    COMMAND sh -c "${e2e_workdir}/interp_logic_app ${e2e_interplc_program} | grep -qx 'and tf: false' && ${e2e_workdir}/interp_logic_app ${e2e_interplc_program} | grep -qx 'short or: true' && ${e2e_workdir}/interp_logic_app ${e2e_interplc_program} | grep -qx 'filtered: 0,2,4'")
set_tests_properties(e2e_interp_logic_demo_verify PROPERTIES DEPENDS e2e_interp_logic_demo_compile)

# Unary logical not: completes the boolean trio (and / or / not). A new pNot level
# between pAnd and pEq, so not binds above and but below the comparisons (not a == b
# is not (a == b)); pNot recurses for not not x. The committed program checks
# negation, double negation, precedence with and / or and ==, composition with a
# builtin, and use in a user predicate.
set(e2e_interpnt_input   ${CMAKE_SOURCE_DIR}/examples/interp_not_demo.vst)
set(e2e_interpnt_program ${CMAKE_SOURCE_DIR}/tests/e2e/interp_not_program.calc)

add_test(
    NAME    e2e_interp_not_demo_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_interpnt_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_interp_not_demo_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_workdir}/interp_not_demo.cpp
            -o ${e2e_workdir}/interp_not_app)
set_tests_properties(e2e_interp_not_demo_compile PROPERTIES DEPENDS e2e_interp_not_demo_transpile)

# Run the not interpreter on the committed program; final `result: done`.
add_test(
    NAME    e2e_interp_not_demo_run
    COMMAND ${e2e_workdir}/interp_not_app ${e2e_interpnt_program})
set_tests_properties(e2e_interp_not_demo_run PROPERTIES
    DEPENDS                 e2e_interp_not_demo_compile
    PASS_REGULAR_EXPRESSION "result: done")

# Verify not reached stdout: a negation (not f: true), precedence with and
# (with and: true), and a not-based user predicate filter (odds: 1,3,5,7).
add_test(
    NAME    e2e_interp_not_demo_verify
    COMMAND sh -c "${e2e_workdir}/interp_not_app ${e2e_interpnt_program} | grep -qx 'not f: true' && ${e2e_workdir}/interp_not_app ${e2e_interpnt_program} | grep -qx 'with and: true' && ${e2e_workdir}/interp_not_app ${e2e_interpnt_program} | grep -qx 'odds: 1,3,5,7'")
set_tests_properties(e2e_interp_not_demo_verify PROPERTIES DEPENDS e2e_interp_not_demo_compile)

# chars(s): explode a string into a list of one-character strings (over slice), the
# bridge from string to list at character granularity. join(chars(s), "") rebuilds s.
# The committed program lists characters, reverses a string via chars+reverse+join,
# and filters/counts vowels with a user predicate using the new or operator.
set(e2e_interpch_input   ${CMAKE_SOURCE_DIR}/examples/interp_chars_demo.vst)
set(e2e_interpch_program ${CMAKE_SOURCE_DIR}/tests/e2e/interp_chars_program.calc)

add_test(
    NAME    e2e_interp_chars_demo_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_interpch_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_interp_chars_demo_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_workdir}/interp_chars_demo.cpp
            -o ${e2e_workdir}/interp_chars_app)
set_tests_properties(e2e_interp_chars_demo_compile PROPERTIES DEPENDS e2e_interp_chars_demo_transpile)

# Run the chars interpreter on the committed program; final `result: done`.
add_test(
    NAME    e2e_interp_chars_demo_run
    COMMAND ${e2e_workdir}/interp_chars_app ${e2e_interpch_program})
set_tests_properties(e2e_interp_chars_demo_run PROPERTIES
    DEPENDS                 e2e_interp_chars_demo_compile
    PASS_REGULAR_EXPRESSION "result: done")

# Verify chars reached stdout: the character list (chars: h,e,l,l,o), a string
# reversed via chars+reverse+join (reversed: cba), and a vowel filter (vowels: euaio).
add_test(
    NAME    e2e_interp_chars_demo_verify
    COMMAND sh -c "${e2e_workdir}/interp_chars_app ${e2e_interpch_program} | grep -qx 'chars: h,e,l,l,o' && ${e2e_workdir}/interp_chars_app ${e2e_interpch_program} | grep -qx 'reversed: cba' && ${e2e_workdir}/interp_chars_app ${e2e_interpch_program} | grep -qx 'vowels: euaio'")
set_tests_properties(e2e_interp_chars_demo_verify PROPERTIES DEPENDS e2e_interp_chars_demo_compile)

# Second capstone: a CSV-to-records pipeline on the complete interpreter
# (interp_chars_demo.vst, the full builtin set). No new interpreter code; the program
# parses rows / fields with split, zips the header over each row into a record map
# with zipMap, then queries columns and aggregates (toInt + sum) -- both compound
# types working together on structured data.
set(e2e_interpcsv_input   ${CMAKE_SOURCE_DIR}/examples/interp_chars_demo.vst)
set(e2e_interpcsv_program ${CMAKE_SOURCE_DIR}/tests/e2e/interp_csv_program.calc)

add_test(
    NAME    e2e_interp_csv_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_interpcsv_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_interp_csv_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_workdir}/interp_chars_demo.cpp
            -o ${e2e_workdir}/interp_csv_app)
set_tests_properties(e2e_interp_csv_compile PROPERTIES DEPENDS e2e_interp_csv_transpile)

# Run the complete interpreter on the committed CSV program; final `report done`.
add_test(
    NAME    e2e_interp_csv_run
    COMMAND ${e2e_workdir}/interp_csv_app ${e2e_interpcsv_program})
set_tests_properties(e2e_interp_csv_run PROPERTIES
    DEPENDS                 e2e_interp_csv_compile
    PASS_REGULAR_EXPRESSION "result: report done")

# Verify the records pipeline reached stdout: a field query (alice age: 30), an
# extracted+sorted column (names: alice,bob,carol), and a numeric aggregate
# (total age: 90).
add_test(
    NAME    e2e_interp_csv_verify
    COMMAND sh -c "${e2e_workdir}/interp_csv_app ${e2e_interpcsv_program} | grep -qx 'alice age: 30' && ${e2e_workdir}/interp_csv_app ${e2e_interpcsv_program} | grep -qx 'names: alice,bob,carol' && ${e2e_workdir}/interp_csv_app ${e2e_interpcsv_program} | grep -qx 'total age: 90'")
set_tests_properties(e2e_interp_csv_verify PROPERTIES DEPENDS e2e_interp_csv_compile)

# sortBy(xs, fn): sort a list by a key function (higher-order generalization of
# sort), resolved through callFunc, keys compared with the type-aware lessVal.
# Unlocks orderings sort cannot express. The committed program sorts ascending (via
# identity), descending (via a negated key), and by string length.
set(e2e_interpsby_input   ${CMAKE_SOURCE_DIR}/examples/interp_sortby_demo.vst)
set(e2e_interpsby_program ${CMAKE_SOURCE_DIR}/tests/e2e/interp_sortby_program.calc)

add_test(
    NAME    e2e_interp_sortby_demo_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_interpsby_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_interp_sortby_demo_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_workdir}/interp_sortby_demo.cpp
            -o ${e2e_workdir}/interp_sortby_app)
set_tests_properties(e2e_interp_sortby_demo_compile PROPERTIES DEPENDS e2e_interp_sortby_demo_transpile)

# Run the sortBy interpreter on the committed program; final `result: done`.
add_test(
    NAME    e2e_interp_sortby_demo_run
    COMMAND ${e2e_workdir}/interp_sortby_app ${e2e_interpsby_program})
set_tests_properties(e2e_interp_sortby_demo_run PROPERTIES
    DEPENDS                 e2e_interp_sortby_demo_compile
    PASS_REGULAR_EXPRESSION "result: done")

# Verify sortBy reached stdout: a descending sort via a negated key
# (descending: 5,3,2,1) and a by-length sort (by length: fig,kiwi,cherry,banana).
add_test(
    NAME    e2e_interp_sortby_demo_verify
    COMMAND sh -c "${e2e_workdir}/interp_sortby_app ${e2e_interpsby_program} | grep -qx 'descending: 5,3,2,1' && ${e2e_workdir}/interp_sortby_app ${e2e_interpsby_program} | grep -qx 'by length: fig,kiwi,cherry,banana'")
set_tests_properties(e2e_interp_sortby_demo_verify PROPERTIES DEPENDS e2e_interp_sortby_demo_compile)

# minBy(xs, fn) / maxBy(xs, fn): the element with the least / greatest key(element),
# the reduce-to-extreme counterparts of sortBy (and by-key min / max). They return
# the element, not the key, comparing keys with lessVal. The committed program finds
# the longest / shortest word, the min / max number, and a max via a negated key.
set(e2e_interpeb_input   ${CMAKE_SOURCE_DIR}/examples/interp_extremeby_demo.vst)
set(e2e_interpeb_program ${CMAKE_SOURCE_DIR}/tests/e2e/interp_extremeby_program.calc)

add_test(
    NAME    e2e_interp_extremeby_demo_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_interpeb_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_interp_extremeby_demo_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_workdir}/interp_extremeby_demo.cpp
            -o ${e2e_workdir}/interp_extremeby_app)
set_tests_properties(e2e_interp_extremeby_demo_compile PROPERTIES DEPENDS e2e_interp_extremeby_demo_transpile)

# Run the minBy/maxBy interpreter on the committed program; final `result: done`.
add_test(
    NAME    e2e_interp_extremeby_demo_run
    COMMAND ${e2e_workdir}/interp_extremeby_app ${e2e_interpeb_program})
set_tests_properties(e2e_interp_extremeby_demo_run PROPERTIES
    DEPENDS                 e2e_interp_extremeby_demo_compile
    PASS_REGULAR_EXPRESSION "result: done")

# Verify minBy/maxBy reached stdout: the longest word (longest: banana) and a max
# found via a negated key (max via neg: 50).
add_test(
    NAME    e2e_interp_extremeby_demo_verify
    COMMAND sh -c "${e2e_workdir}/interp_extremeby_app ${e2e_interpeb_program} | grep -qx 'longest: banana' && ${e2e_workdir}/interp_extremeby_app ${e2e_interpeb_program} | grep -qx 'shortest: fig' && ${e2e_workdir}/interp_extremeby_app ${e2e_interpeb_program} | grep -qx 'max via neg: 50'")
set_tests_properties(e2e_interp_extremeby_demo_verify PROPERTIES DEPENDS e2e_interp_extremeby_demo_compile)

# findFirst(xs, fn): the linear-search reducer. It walks the list and returns the
# first element for which the named predicate is truthy, or 0 when none match (and
# for the empty list). The committed program finds the first big number, the first
# value greater than two over a range, and the first vowel in a char list.
set(e2e_interpff_input   ${CMAKE_SOURCE_DIR}/examples/interp_findfirst_demo.vst)
set(e2e_interpff_program ${CMAKE_SOURCE_DIR}/tests/e2e/interp_findfirst_program.calc)

add_test(
    NAME    e2e_interp_findfirst_demo_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_interpff_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_interp_findfirst_demo_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_workdir}/interp_findfirst_demo.cpp
            -o ${e2e_workdir}/interp_findfirst_app)
set_tests_properties(e2e_interp_findfirst_demo_compile PROPERTIES DEPENDS e2e_interp_findfirst_demo_transpile)

# Run the findFirst interpreter on the committed program; final `result: done`.
add_test(
    NAME    e2e_interp_findfirst_demo_run
    COMMAND ${e2e_workdir}/interp_findfirst_app ${e2e_interpff_program})
set_tests_properties(e2e_interp_findfirst_demo_run PROPERTIES
    DEPENDS                 e2e_interp_findfirst_demo_compile
    PASS_REGULAR_EXPRESSION "result: done")

# Verify findFirst reached stdout: the first big number (first big: 30), the first
# value over two in a range (first gt2: 3), and the first vowel (first vowel: e).
add_test(
    NAME    e2e_interp_findfirst_demo_verify
    COMMAND sh -c "${e2e_workdir}/interp_findfirst_app ${e2e_interpff_program} | grep -qx 'first big: 30' && ${e2e_workdir}/interp_findfirst_app ${e2e_interpff_program} | grep -qx 'first gt2: 3' && ${e2e_workdir}/interp_findfirst_app ${e2e_interpff_program} | grep -qx 'first vowel: e'")
set_tests_properties(e2e_interp_findfirst_demo_verify PROPERTIES DEPENDS e2e_interp_findfirst_demo_compile)

# count(xs, fn): the tally counterpart of findFirst. It scans the whole list and
# returns how many elements make the named predicate truthy (0 for the empty list).
# The committed program counts the big numbers, the vowels in a char list, and the
# even values over a range.
set(e2e_interpct_input   ${CMAKE_SOURCE_DIR}/examples/interp_count_demo.vst)
set(e2e_interpct_program ${CMAKE_SOURCE_DIR}/tests/e2e/interp_count_program.calc)

add_test(
    NAME    e2e_interp_count_demo_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_interpct_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_interp_count_demo_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_workdir}/interp_count_demo.cpp
            -o ${e2e_workdir}/interp_count_app)
set_tests_properties(e2e_interp_count_demo_compile PROPERTIES DEPENDS e2e_interp_count_demo_transpile)

# Run the count interpreter on the committed program; final `result: done`.
add_test(
    NAME    e2e_interp_count_demo_run
    COMMAND ${e2e_workdir}/interp_count_app ${e2e_interpct_program})
set_tests_properties(e2e_interp_count_demo_run PROPERTIES
    DEPENDS                 e2e_interp_count_demo_compile
    PASS_REGULAR_EXPRESSION "result: done")

# Verify count reached stdout: the big numbers (big count: 2), the vowels in a char
# list (vowels: 2), and the even values over a range (evens: 5).
add_test(
    NAME    e2e_interp_count_demo_verify
    COMMAND sh -c "${e2e_workdir}/interp_count_app ${e2e_interpct_program} | grep -qx 'big count: 2' && ${e2e_workdir}/interp_count_app ${e2e_interpct_program} | grep -qx 'vowels: 2' && ${e2e_workdir}/interp_count_app ${e2e_interpct_program} | grep -qx 'evens: 5'")
set_tests_properties(e2e_interp_count_demo_verify PROPERTIES DEPENDS e2e_interp_count_demo_compile)

# product(xs): the multiplicative fold, the companion of sum. It multiplies the list
# elements starting from 1 (so the empty list yields the identity 1). The committed
# program sums and multiplies a small list and computes 5! as product(1..5).
set(e2e_interppr_input   ${CMAKE_SOURCE_DIR}/examples/interp_product_demo.vst)
set(e2e_interppr_program ${CMAKE_SOURCE_DIR}/tests/e2e/interp_product_program.calc)

add_test(
    NAME    e2e_interp_product_demo_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_interppr_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_interp_product_demo_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_workdir}/interp_product_demo.cpp
            -o ${e2e_workdir}/interp_product_app)
set_tests_properties(e2e_interp_product_demo_compile PROPERTIES DEPENDS e2e_interp_product_demo_transpile)

# Run the product interpreter on the committed program; final `result: done`.
add_test(
    NAME    e2e_interp_product_demo_run
    COMMAND ${e2e_workdir}/interp_product_app ${e2e_interppr_program})
set_tests_properties(e2e_interp_product_demo_run PROPERTIES
    DEPENDS                 e2e_interp_product_demo_compile
    PASS_REGULAR_EXPRESSION "result: done")

# Verify product reached stdout: the multiplied list (product: 24), the factorial
# (factorial5: 120), and the empty-list identity (empty: 1).
add_test(
    NAME    e2e_interp_product_demo_verify
    COMMAND sh -c "${e2e_workdir}/interp_product_app ${e2e_interppr_program} | grep -qx 'product: 24' && ${e2e_workdir}/interp_product_app ${e2e_interppr_program} | grep -qx 'factorial5: 120' && ${e2e_workdir}/interp_product_app ${e2e_interppr_program} | grep -qx 'empty: 1'")
set_tests_properties(e2e_interp_product_demo_verify PROPERTIES DEPENDS e2e_interp_product_demo_compile)

# avg(xs): the integer mean, sum(xs) / len(xs), truncating toward zero. It guards the
# empty list (returns 0, no divide by zero). The committed program averages a list,
# checks the truncating division over a range, and the empty case.
set(e2e_interpav_input   ${CMAKE_SOURCE_DIR}/examples/interp_avg_demo.vst)
set(e2e_interpav_program ${CMAKE_SOURCE_DIR}/tests/e2e/interp_avg_program.calc)

add_test(
    NAME    e2e_interp_avg_demo_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_interpav_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_interp_avg_demo_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_workdir}/interp_avg_demo.cpp
            -o ${e2e_workdir}/interp_avg_app)
set_tests_properties(e2e_interp_avg_demo_compile PROPERTIES DEPENDS e2e_interp_avg_demo_transpile)

# Run the avg interpreter on the committed program; final `result: done`.
add_test(
    NAME    e2e_interp_avg_demo_run
    COMMAND ${e2e_workdir}/interp_avg_app ${e2e_interpav_program})
set_tests_properties(e2e_interp_avg_demo_run PROPERTIES
    DEPENDS                 e2e_interp_avg_demo_compile
    PASS_REGULAR_EXPRESSION "result: done")

# Verify avg reached stdout: the mean of a list (avg: 25), the truncating mean over a
# range (trunc: 4), and the empty-list guard (empty: 0).
add_test(
    NAME    e2e_interp_avg_demo_verify
    COMMAND sh -c "${e2e_workdir}/interp_avg_app ${e2e_interpav_program} | grep -qx 'avg: 25' && ${e2e_workdir}/interp_avg_app ${e2e_interpav_program} | grep -qx 'trunc: 4' && ${e2e_workdir}/interp_avg_app ${e2e_interpav_program} | grep -qx 'empty: 0'")
set_tests_properties(e2e_interp_avg_demo_verify PROPERTIES DEPENDS e2e_interp_avg_demo_compile)

# unique(xs): a new list keeping each distinct element once, in first-seen order,
# with equality by valEq (the member / indexOf notion). The committed program dedupes
# a list, counts distinct values, and combines with sort for a sorted distinct set.
set(e2e_interpun_input   ${CMAKE_SOURCE_DIR}/examples/interp_unique_demo.vst)
set(e2e_interpun_program ${CMAKE_SOURCE_DIR}/tests/e2e/interp_unique_program.calc)

add_test(
    NAME    e2e_interp_unique_demo_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_interpun_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_interp_unique_demo_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_workdir}/interp_unique_demo.cpp
            -o ${e2e_workdir}/interp_unique_app)
set_tests_properties(e2e_interp_unique_demo_compile PROPERTIES DEPENDS e2e_interp_unique_demo_transpile)

# Run the unique interpreter on the committed program; final `result: done`.
add_test(
    NAME    e2e_interp_unique_demo_run
    COMMAND ${e2e_workdir}/interp_unique_app ${e2e_interpun_program})
set_tests_properties(e2e_interp_unique_demo_run PROPERTIES
    DEPENDS                 e2e_interp_unique_demo_compile
    PASS_REGULAR_EXPRESSION "result: done")

# Verify unique reached stdout: the deduped list (unique: 1,2,3), the distinct letter
# count (letters: 4), and the sorted distinct set (sorted set: 1,2,3).
add_test(
    NAME    e2e_interp_unique_demo_verify
    COMMAND sh -c "${e2e_workdir}/interp_unique_app ${e2e_interpun_program} | grep -qx 'unique: 1,2,3' && ${e2e_workdir}/interp_unique_app ${e2e_interpun_program} | grep -qx 'letters: 4' && ${e2e_workdir}/interp_unique_app ${e2e_interpun_program} | grep -qx 'sorted set: 1,2,3'")
set_tests_properties(e2e_interp_unique_demo_verify PROPERTIES DEPENDS e2e_interp_unique_demo_compile)

# A richer value type: a tagged Value enum (int / bool / str). Literals carry a
# type, arithmetic stays int, comparison / == produce bool, and `if` may pick
# between branches of different types; `format` renders by tag.
set(e2e_calcval_input  ${CMAKE_SOURCE_DIR}/examples/calc_value_demo.vst)
set(e2e_calcval_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_calc_value_demo.cpp)

add_test(
    NAME    e2e_calc_value_demo_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_calcval_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_calc_value_demo_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_calcval_driver} ${e2e_workdir}/calc_value_demo.cpp
            -o ${e2e_workdir}/calc_value_demo_app)
set_tests_properties(e2e_calc_value_demo_compile PROPERTIES DEPENDS e2e_calc_value_demo_transpile)

add_test(
    NAME    e2e_calc_value_demo_run
    COMMAND ${e2e_workdir}/calc_value_demo_app)
set_tests_properties(e2e_calc_value_demo_run PROPERTIES
    DEPENDS                 e2e_calc_value_demo_compile
    PASS_REGULAR_EXPRESSION "calc_value_demo OK")

# Self-hosting proof-of-concept: a standalone `func main` program that reads a
# file (argv + readFile), tokenizes with split, tabulates with Vec + HashMap,
# renders a report with String + toString, writes it (writeFile), and echoes it
# (print). Drives the whole self-hosting equipment stack end to end. The run
# asserts the summary on stdout; a verify step greps the written output file.
set(e2e_selfhost_input   ${CMAKE_SOURCE_DIR}/examples/selfhost_demo.vst)
set(e2e_selfhost_fixture ${CMAKE_SOURCE_DIR}/tests/e2e/selfhost_input.txt)

add_test(
    NAME    e2e_selfhost_demo_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_selfhost_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_selfhost_demo_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_workdir}/selfhost_demo.cpp
            -o ${e2e_workdir}/selfhost_app)
set_tests_properties(e2e_selfhost_demo_compile PROPERTIES DEPENDS e2e_selfhost_demo_transpile)

add_test(
    NAME    e2e_selfhost_demo_run
    COMMAND ${e2e_workdir}/selfhost_app ${e2e_selfhost_fixture} ${e2e_workdir}/selfhost_out.txt)
set_tests_properties(e2e_selfhost_demo_run PROPERTIES
    DEPENDS                e2e_selfhost_demo_compile
    PASS_REGULAR_EXPRESSION "words: 14\nunique: 10\nsum: 42")

# Verify the written report file directly (the run only proves stdout).
add_test(
    NAME    e2e_selfhost_demo_verify
    COMMAND grep -F "the: 4" ${e2e_workdir}/selfhost_out.txt)
set_tests_properties(e2e_selfhost_demo_verify PROPERTIES DEPENDS e2e_selfhost_demo_run)

# §5/§18.4 split(at:) partition primitive.
set(e2e_split_input  ${CMAKE_SOURCE_DIR}/examples/split_demo.vst)
set(e2e_split_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_split_demo.cpp)

add_test(
    NAME    e2e_split_demo_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_split_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_split_demo_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_split_driver} ${e2e_workdir}/split_demo.cpp
            -o ${e2e_workdir}/split_demo_app)
set_tests_properties(e2e_split_demo_compile PROPERTIES DEPENDS e2e_split_demo_transpile)

add_test(
    NAME    e2e_split_demo_run
    COMMAND ${e2e_workdir}/split_demo_app)
set_tests_properties(e2e_split_demo_run PROPERTIES
    DEPENDS                e2e_split_demo_compile
    PASS_REGULAR_EXPRESSION "split_demo OK")

# §5/§18.4 chunks(of:) partition primitive.
set(e2e_chunks_input  ${CMAKE_SOURCE_DIR}/examples/chunks_demo.vst)
set(e2e_chunks_driver ${CMAKE_SOURCE_DIR}/tests/e2e/main_chunks_demo.cpp)

add_test(
    NAME    e2e_chunks_demo_transpile
    COMMAND $<TARGET_FILE:vestra> build ${e2e_chunks_input} -o ${e2e_workdir})

add_test(
    NAME    e2e_chunks_demo_compile
    COMMAND ${CMAKE_CXX_COMPILER}
            -std=c++26 -Wall -Wextra -O0
            -I${e2e_workdir}
            ${e2e_chunks_driver} ${e2e_workdir}/chunks_demo.cpp
            -o ${e2e_workdir}/chunks_demo_app)
set_tests_properties(e2e_chunks_demo_compile PROPERTIES DEPENDS e2e_chunks_demo_transpile)

add_test(
    NAME    e2e_chunks_demo_run
    COMMAND ${e2e_workdir}/chunks_demo_app)
set_tests_properties(e2e_chunks_demo_run PROPERTIES
    DEPENDS                e2e_chunks_demo_compile
    PASS_REGULAR_EXPRESSION "chunks_demo OK")
