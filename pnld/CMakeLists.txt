cmake_minimum_required(VERSION 3.21)
project(pnld LANGUAGES CXX)

# When configured standalone (not via the solution root), pull in the shared
# toolchain config and the common module.
if(NOT TARGET hft::common)
  include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/HftConfig.cmake)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../common ${CMAKE_CURRENT_BINARY_DIR}/common)
endif()

# Headers listed explicitly so IDE generators show them in the project tree.
set(PL_HEADERS
  include/pl/types.hpp
  include/pl/book.hpp
  include/pl/keeper.hpp
  include/pl/sim.hpp
)

set(PL_DOCS README.md)

add_executable(pnld src/main.cpp ${PL_HEADERS} ${PL_DOCS})
hft_configure(pnld)

add_executable(pnld_tests src/tests.cpp ${PL_HEADERS})
hft_configure(pnld_tests)

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}
             FILES src/main.cpp src/tests.cpp ${PL_HEADERS} ${PL_DOCS})
