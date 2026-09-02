cmake_minimum_required(VERSION 3.21)
project(feedarb LANGUAGES CXX)

# When configured standalone (not via the solution root), pull in the shared
# toolchain config and the common module.
if(NOT TARGET hft::common)
  include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/HftConfig.cmake)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../common ${CMAKE_CURRENT_BINARY_DIR}/common)
endif()

# Headers listed explicitly so IDE generators show them in the project tree.
set(FA_HEADERS
  include/fa/core/packet.hpp
  include/fa/core/arbiter.hpp
  include/fa/sim/line_sim.hpp
)

set(FA_DOCS README.md)

add_executable(feedarb src/main.cpp ${FA_HEADERS} ${FA_DOCS})
hft_configure(feedarb)

add_executable(feedarb_tests src/tests.cpp ${FA_HEADERS})
hft_configure(feedarb_tests)

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}
             FILES src/main.cpp src/tests.cpp ${FA_HEADERS} ${FA_DOCS})
