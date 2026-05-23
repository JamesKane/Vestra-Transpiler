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
    codegen/cpp_emitter_test.cpp)

add_executable(vestra_tests
    main.cpp
    ${test_sources})

target_link_libraries(vestra_tests
    PRIVATE
        vestra_core
        doctest::doctest)

target_compile_features(vestra_tests PRIVATE cxx_std_23)
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
            -std=c++23 -Wall -Wextra -O0
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
            -std=c++23 -Wall -Wextra -O0
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
            -std=c++23 -Wall -Wextra -O0
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
