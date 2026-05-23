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
