cmake_minimum_required(VERSION 3.21)
project(seqbus LANGUAGES CXX)

# When configured standalone (not via the solution root), pull in the shared
# toolchain config and the common module.
if(NOT TARGET hft::common)
  include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/HftConfig.cmake)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../common ${CMAKE_CURRENT_BINARY_DIR}/common)
endif()

# Headers listed explicitly so IDE generators show them in the project tree.
set(SB_HEADERS
  include/sb/core/shm.hpp
  include/sb/core/record.hpp
  include/sb/core/bus.hpp
  include/sb/harness/msg.hpp
)

set(SB_DOCS README.md)

add_executable(seqbus src/main.cpp ${SB_HEADERS} ${SB_DOCS})
hft_configure(seqbus)

add_executable(seqbus_tests src/tests.cpp ${SB_HEADERS})
hft_configure(seqbus_tests)

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}
             FILES src/main.cpp src/tests.cpp ${SB_HEADERS} ${SB_DOCS})
