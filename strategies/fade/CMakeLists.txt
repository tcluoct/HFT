cmake_minimum_required(VERSION 3.21)
project(strat_fade LANGUAGES CXX)

# strat_fade — the fade team's static library: snapshot mean-reversion.
# Depends on the platform's strategy SDK (strat_sdk), never on the engine.
#
# Standalone: cmake -S strategies/fade -B build/fade && cmake --build build/fade
if(NOT COMMAND hft_configure)
  include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/HftConfig.cmake)
endif()
if(NOT TARGET strat_sdk)
  set(HFT_PLATFORM_APPS OFF CACHE BOOL "Build the platform executables" FORCE)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../platform ${CMAKE_BINARY_DIR}/platform)
endif()

add_library(strat_fade STATIC
  src/anchor.cpp
  include/strat/fade/meanrev.hpp)
target_include_directories(strat_fade PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(strat_fade PUBLIC strat_sdk)
hft_configure_lib(strat_fade)
set_target_properties(strat_fade PROPERTIES FOLDER "strategies")

add_executable(strat_fade_tests src/tests.cpp)
hft_configure(strat_fade_tests)
target_link_libraries(strat_fade_tests PRIVATE strat_fade platform_lib)
set_target_properties(strat_fade_tests PROPERTIES FOLDER "strategies")

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES
  src/anchor.cpp src/tests.cpp include/strat/fade/meanrev.hpp)
