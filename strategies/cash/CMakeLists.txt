cmake_minimum_required(VERSION 3.21)
project(strat_cash LANGUAGES CXX)

# strat_cash — the cash-equity team's static library: short-horizon L2 alpha
# on carried stock inventory (T+1 / 底仓). Depends on the platform's strategy
# SDK (strat_sdk), never on the engine. Phase 2: the house desk does not field
# it yet (no stock venue in Phase 1); its own suite keeps it compiling.
#
# Standalone: cmake -S strategies/cash -B build/cash && cmake --build build/cash
if(NOT COMMAND hft_configure)
  include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/HftConfig.cmake)
endif()
if(NOT TARGET strat_sdk)
  set(HFT_PLATFORM_APPS OFF CACHE BOOL "Build the platform executables" FORCE)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../platform ${CMAKE_BINARY_DIR}/platform)
endif()

add_library(strat_cash STATIC
  src/anchor.cpp
  include/strat/cash/t0alpha.hpp)
target_include_directories(strat_cash PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(strat_cash PUBLIC strat_sdk)
hft_configure_lib(strat_cash)
set_target_properties(strat_cash PROPERTIES FOLDER "strategies")

add_executable(strat_cash_tests src/tests.cpp)
hft_configure(strat_cash_tests)
target_link_libraries(strat_cash_tests PRIVATE strat_cash platform_lib)
set_target_properties(strat_cash_tests PROPERTIES FOLDER "strategies")

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES
  src/anchor.cpp src/tests.cpp include/strat/cash/t0alpha.hpp)
