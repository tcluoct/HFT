cmake_minimum_required(VERSION 3.21)
project(strat_basis LANGUAGES CXX)

# strat_basis — the basis-arbitrage team's static library: the futures hedge
# leg and the cash leg, coordinating only through cross-site messages.
# Depends on the platform's strategy SDK (strat_sdk), never on the engine.
# Phase 2: not fielded in the house desk's Pack (no federation venue yet), so
# the desk does not link it; the team's own suite keeps it compiling.
#
# Standalone: cmake -S strategies/basis -B build/basis && cmake --build build/basis
if(NOT COMMAND hft_configure)
  include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/HftConfig.cmake)
endif()
if(NOT TARGET strat_sdk)
  set(HFT_PLATFORM_APPS OFF CACHE BOOL "Build the platform executables" FORCE)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../platform ${CMAKE_BINARY_DIR}/platform)
endif()

add_library(strat_basis STATIC
  src/anchor.cpp
  include/strat/basis/basisarb.hpp)
target_include_directories(strat_basis PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(strat_basis PUBLIC strat_sdk)
hft_configure_lib(strat_basis)
set_target_properties(strat_basis PROPERTIES FOLDER "strategies")

add_executable(strat_basis_tests src/tests.cpp)
hft_configure(strat_basis_tests)
target_link_libraries(strat_basis_tests PRIVATE strat_basis platform_lib)
set_target_properties(strat_basis_tests PROPERTIES FOLDER "strategies")

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES
  src/anchor.cpp src/tests.cpp include/strat/basis/basisarb.hpp)
