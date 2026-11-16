cmake_minimum_required(VERSION 3.21)
project(strat_trend LANGUAGES CXX)

# strat_trend — the trend team's static library: snapshot-momentum and
# session-breakout. Depends on the platform's strategy SDK (strat_sdk), never
# on the engine; the desk project composes it into a deployable lineup.
#
# Standalone: cmake -S strategies/trend -B build/trend && cmake --build build/trend
if(NOT COMMAND hft_configure)
  include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/HftConfig.cmake)
endif()
if(NOT TARGET strat_sdk)
  set(HFT_PLATFORM_APPS OFF CACHE BOOL "Build the platform executables" FORCE)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../platform ${CMAKE_BINARY_DIR}/platform)
endif()

add_library(strat_trend STATIC
  src/anchor.cpp
  include/strat/trend/momentum.hpp
  include/strat/trend/breakout.hpp)
target_include_directories(strat_trend PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(strat_trend PUBLIC strat_sdk)
hft_configure_lib(strat_trend)
set_target_properties(strat_trend PROPERTIES FOLDER "strategies")

# The team's suite: strategy behaviour only; the engine is just the host.
add_executable(strat_trend_tests src/tests.cpp)
hft_configure(strat_trend_tests)
target_link_libraries(strat_trend_tests PRIVATE strat_trend platform_lib)
set_target_properties(strat_trend_tests PROPERTIES FOLDER "strategies")

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES
  src/anchor.cpp src/tests.cpp
  include/strat/trend/momentum.hpp include/strat/trend/breakout.hpp)
