cmake_minimum_required(VERSION 3.21)
project(desk LANGUAGES CXX)

# desk — the composition root: it names the strategy-team libraries this desk
# deploys and exports the derived Pack / PackSite / parse machinery
# (desk/pack.hpp). The platform engine knows no strategy; the platform apps
# link THIS library and nothing else strategy-specific.
#
# Building a different desk is a new header here plus a target that links the
# team libraries it fields — no platform change.
#
# Standalone: cmake -S desk -B build/desk && cmake --build build/desk
if(NOT COMMAND hft_configure)
  include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/HftConfig.cmake)
endif()
if(NOT TARGET strat_sdk)
  set(HFT_PLATFORM_APPS OFF CACHE BOOL "Build the platform executables" FORCE)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../platform ${CMAKE_BINARY_DIR}/platform)
endif()
if(NOT TARGET strat_trend)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../strategies ${CMAKE_BINARY_DIR}/strategies)
endif()

# The house desk fields three production futures strategies from two teams:
# trend (momentum + breakout) and fade (meanrev). The Phase-2 basis/cash team
# libraries are not in this Pack, so they are not linked here.
add_library(desk STATIC src/anchor.cpp include/desk/pack.hpp)
target_include_directories(desk PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(desk PUBLIC platform_lib strat_trend strat_fade)
hft_configure_lib(desk)
set_target_properties(desk PROPERTIES FOLDER "desk")

# The desk suite: cross-team composition + the config-facing lineup machinery.
add_executable(desk_tests src/tests.cpp)
hft_configure(desk_tests)
target_link_libraries(desk_tests PRIVATE desk)
set_target_properties(desk_tests PROPERTIES FOLDER "desk")

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES
  src/anchor.cpp src/tests.cpp include/desk/pack.hpp)
