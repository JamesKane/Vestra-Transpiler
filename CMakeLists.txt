cmake_minimum_required(VERSION 3.25)

project(vestra
    VERSION     0.1.0
    DESCRIPTION "Vestra-to-C++ transpiler"
    LANGUAGES   CXX)

# ---- Global toolchain settings ---------------------------------------------

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

# Faster repeated builds when ccache is installed.
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
endif()

list(PREPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(CompilerWarnings)
include(Sanitizers)
include(StaticAnalyzers)
include(Dependencies)

# ---- Options ---------------------------------------------------------------

option(VESTRA_BUILD_TESTS "Build the test suite"      ON)
option(VESTRA_BUILD_TOOLS "Build the vestra CLI tool" ON)

# ---- The library: the transpiler engine -----------------------------------

# Sources are listed explicitly. Globs are convenient but they hide additions
# from the build system, which is exactly the kind of magic this project
# is trying to avoid in the language it implements.
set(vestra_sources
    src/diag/source_manager.cpp
    src/diag/diagnostic.cpp
    src/lex/token.cpp
    src/lex/lexer.cpp
    src/ast/nodes.cpp
    src/ast/printer.cpp
    src/parse/parser.cpp
    src/sema/types.cpp
    src/sema/scope.cpp
    src/sema/resolver.cpp
    src/sema/ownership.cpp
    src/sema/exclusivity.cpp
    src/sema/capability.cpp
    src/sema/comptime.cpp
    src/codegen/cpp_emitter.cpp
    src/driver/driver.cpp)

add_library(vestra_core STATIC ${vestra_sources})
target_include_directories(vestra_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>)
target_compile_features(vestra_core PUBLIC cxx_std_26)
vestra_apply_warnings(vestra_core)
vestra_apply_sanitizers(vestra_core)

# ---- The CLI tool ----------------------------------------------------------

if(VESTRA_BUILD_TOOLS)
    add_executable(vestra src/main.cpp)
    target_link_libraries(vestra PRIVATE vestra_core)
    vestra_apply_warnings(vestra)
    vestra_apply_sanitizers(vestra)
endif()

# ---- Tests -----------------------------------------------------------------

if(VESTRA_BUILD_TESTS)
    enable_testing()
    vestra_fetch_doctest()
    add_subdirectory(tests)
endif()

# ---- Custom targets --------------------------------------------------------

vestra_add_format_target()
vestra_add_tidy_target()

# ---- Build summary ---------------------------------------------------------

message(STATUS "")
message(STATUS "Vestra transpiler ${PROJECT_VERSION}")
message(STATUS "  Build type        : ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ standard      : ${CMAKE_CXX_STANDARD}")
message(STATUS "  Compiler          : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  Tests             : ${VESTRA_BUILD_TESTS}")
message(STATUS "  CLI tool          : ${VESTRA_BUILD_TOOLS}")
message(STATUS "  Sanitizers        : ${VESTRA_SANITIZE}")
message(STATUS "  clang-format      : ${VESTRA_CLANG_FORMAT}")
message(STATUS "  clang-tidy        : ${VESTRA_CLANG_TIDY}")
message(STATUS "  ccache            : ${CCACHE_PROGRAM}")
message(STATUS "")
