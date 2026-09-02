cmake_minimum_required(VERSION 3.21)
project(sigcalc LANGUAGES CXX)

# When configured standalone (not via the solution root), pull in the shared
# toolchain config and the common module.
if(NOT TARGET hft::common)
  include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/HftConfig.cmake)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../common ${CMAKE_CURRENT_BINARY_DIR}/common)
endif()

# Headers listed explicitly so IDE generators show them in the project tree.
set(SG_HEADERS
  include/sg/types.hpp
  include/sg/windows.hpp
  include/sg/ewma.hpp
  include/sg/engine.hpp
  include/sg/sim.hpp
)

set(SG_DOCS README.md)

add_executable(sigcalc src/main.cpp ${SG_HEADERS} ${SG_DOCS})
hft_configure(sigcalc)

add_executable(sigcalc_tests src/tests.cpp ${SG_HEADERS})
hft_configure(sigcalc_tests)

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}
             FILES src/main.cpp src/tests.cpp ${SG_HEADERS} ${SG_DOCS})
