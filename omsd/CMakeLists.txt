cmake_minimum_required(VERSION 3.21)
project(omsd LANGUAGES CXX)

# When configured standalone (not via the solution root), pull in the shared
# toolchain config and the common module.
if(NOT TARGET hft::common)
  include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/HftConfig.cmake)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../common ${CMAKE_CURRENT_BINARY_DIR}/common)
endif()

# Headers listed explicitly so IDE generators show them in the project tree.
set(OM_HEADERS
  include/om/core/types.hpp
  include/om/core/oms.hpp
  include/om/core/recon.hpp
  include/om/sim/venue_sim.hpp
)

set(OM_DOCS README.md)

add_executable(omsd src/main.cpp ${OM_HEADERS} ${OM_DOCS})
hft_configure(omsd)

add_executable(omsd_tests src/tests.cpp ${OM_HEADERS})
hft_configure(omsd_tests)

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}
             FILES src/main.cpp src/tests.cpp ${OM_HEADERS} ${OM_DOCS})
