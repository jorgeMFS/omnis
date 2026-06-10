# =============================================================================
# OMNIS — Observational Minimal-length Nonparametric Inductive Synthesis
# =============================================================================
# Build:
#   mkdir build-cmake && cd build-cmake
#   cmake .. -DCMAKE_BUILD_TYPE=Release
#   make -j
# Test:
#   make test     # runs the four canonical regression tests via CTest
# =============================================================================

cmake_minimum_required(VERSION 3.16)

project(omnis
    VERSION 0.1.0
    DESCRIPTION "Observational Minimal-length Nonparametric Inductive Synthesis"
    LANGUAGES CXX
)

# C++17 required (decomposition declarations, structured bindings, etc.)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Default to Release build if not specified.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

# Optimization flags. The engine is performance-sensitive (multi-thread search,
# tight inner loops). -O3 + native arch are the production target.
add_compile_options(
    $<$<CONFIG:Release>:-O3>
    $<$<CONFIG:Release>:-march=native>
    -Wall
)

# Suppress the C++17-extension warnings on clang. They are pre-existing and
# benign; the code is C++17-compliant.
add_compile_options(-Wno-c++17-extensions)

# Threading (used by Phase 2 worker pools).
find_package(Threads REQUIRED)

# Source directory.
set(OMNIS_SRC ${CMAKE_SOURCE_DIR}/src)
include_directories(${OMNIS_SRC})

# -----------------------------------------------------------------------------
# Targets
# -----------------------------------------------------------------------------

# omnis: minimal CLI wrapping the engine. Reads one sequence from a file or
# stdin, runs solve(), prints the discovered program. Engine code is in
# omnis.cpp; cli.cpp #includes it directly.
add_executable(omnis ${OMNIS_SRC}/cli.cpp)
target_link_libraries(omnis PRIVATE Threads::Threads)

# gen_workload: deterministic categorical workload generator. Pure C++17,
# no engine link. Emits omnis-workload v1 format to stdout. See
# tools/gen_workload.cpp and docs/INTEGRATION_PLAN.md (Phase A) for design.
# Compiled at -O2 — data emission is not perf-critical and -O2 keeps
# debugging tractable for the determinism contract.
set(OMNIS_TOOLS_DIR ${CMAKE_SOURCE_DIR}/tools)
add_executable(gen_workload ${OMNIS_TOOLS_DIR}/gen_workload.cpp)
target_compile_options(gen_workload PRIVATE -O2)

# oeis_loader: streams the pinned OEIS snapshot, applies a named filter,
# emits omnis-workload v1. Requires zlib (for stripped.gz / names.gz).
find_package(ZLIB REQUIRED)
add_executable(oeis_loader ${OMNIS_TOOLS_DIR}/oeis_loader.cpp)
target_compile_options(oeis_loader PRIVATE -O2)
target_link_libraries(oeis_loader PRIVATE ZLIB::ZLIB)

# omnis_validate: train/test wrapper around the engine. Reads a candidate,
# splits last K = max(20, N/4) terms as held-out, runs solve() on training
# prefix, calls runProgram (or predictNext for CTX mode) to extend,
# classifies into the Solomonoff contingency cell, emits one CSV data row.
add_executable(omnis_validate ${OMNIS_TOOLS_DIR}/omnis_validate.cpp)
target_link_libraries(omnis_validate PRIVATE Threads::Threads)

# -----------------------------------------------------------------------------
# Tests (CTest)
# -----------------------------------------------------------------------------

enable_testing()

set(OMNIS_TESTS_DIR ${CMAKE_SOURCE_DIR}/tests)

# test_omega_sub_call: SUB_CALL semantics, 8 tests
add_executable(test_omega_sub_call ${OMNIS_TESTS_DIR}/test_omega_sub_call.cpp)
target_link_libraries(test_omega_sub_call PRIVATE Threads::Threads)
add_test(NAME test_omega_sub_call COMMAND test_omega_sub_call)

# test_phase2h_simple: Phase 2H pool building, 4 tests
add_executable(test_phase2h_simple ${OMNIS_TESTS_DIR}/test_phase2h_simple.cpp)
target_include_directories(test_phase2h_simple PRIVATE ${OMNIS_TESTS_DIR})
target_link_libraries(test_phase2h_simple PRIVATE Threads::Threads)
add_test(NAME test_phase2h_simple COMMAND test_phase2h_simple)

# test_phase2f_budget: Phase 2F budget enforcement, 3 tests (~3 min)
add_executable(test_phase2f_budget ${OMNIS_TESTS_DIR}/test_phase2f_budget.cpp)
target_include_directories(test_phase2f_budget PRIVATE ${OMNIS_TESTS_DIR})
target_link_libraries(test_phase2f_budget PRIVATE Threads::Threads)
add_test(NAME test_phase2f_budget COMMAND test_phase2f_budget)
set_tests_properties(test_phase2f_budget PROPERTIES TIMEOUT 600)

# test_correctness_fixes: regression coverage for audit-found bugs, 11 tests
add_executable(test_correctness_fixes ${OMNIS_TESTS_DIR}/test_correctness_fixes.cpp)
target_link_libraries(test_correctness_fixes PRIVATE Threads::Threads)
add_test(NAME test_correctness_fixes COMMAND test_correctness_fixes)

# test_engine_determinism: 30-candidate × 3-rerun canary asserting the §3
# reproducibility contract — (sc, pred_sc) identical, MDL within ±0.5 bits
# AT SUFFICIENT BUDGET. 60s/run; total ≤ 30 × 3 × 60s = 90 min worst case
# (easy candidates solve in <10s; realistic total ~25-40 min).
add_executable(test_engine_determinism ${OMNIS_TESTS_DIR}/test_engine_determinism.cpp)
target_include_directories(test_engine_determinism PRIVATE ${OMNIS_TESTS_DIR})
target_link_libraries(test_engine_determinism PRIVATE Threads::Threads)
add_test(NAME test_engine_determinism COMMAND test_engine_determinism)
add_dependencies(test_engine_determinism omnis_validate)
set_tests_properties(test_engine_determinism PROPERTIES TIMEOUT 5500)

# test_gen_workload: Phase D regression tests for the categorical pipeline.
# Exercises the gen_workload + omnis_validate binaries via system() and
# verifies committed CHECKSUMS.sha256. Requires gen_workload + omnis_validate
# targets to be built before this test runs.
add_executable(test_gen_workload ${OMNIS_TESTS_DIR}/test_gen_workload.cpp)
target_include_directories(test_gen_workload PRIVATE ${OMNIS_TESTS_DIR})
add_test(NAME test_gen_workload COMMAND test_gen_workload)
add_dependencies(test_gen_workload gen_workload omnis_validate)

# -----------------------------------------------------------------------------
# Install
# -----------------------------------------------------------------------------

install(TARGETS omnis omnis_validate gen_workload oeis_loader
    RUNTIME DESTINATION bin
)
install(FILES
    ${CMAKE_SOURCE_DIR}/LICENSE
    ${CMAKE_SOURCE_DIR}/README.md
    ${CMAKE_SOURCE_DIR}/CITATION.cff
    DESTINATION share/omnis
)
install(DIRECTORY ${CMAKE_SOURCE_DIR}/docs/
    DESTINATION share/omnis/docs
    FILES_MATCHING PATTERN "*.md"
)

message(STATUS "OMNIS configured: ${CMAKE_BUILD_TYPE} build")
message(STATUS "  C++ standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "  Targets: omnis, omnis_validate, gen_workload, oeis_loader")
message(STATUS "  Tests: 5 suites (CTest)")
