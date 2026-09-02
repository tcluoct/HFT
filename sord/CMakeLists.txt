cmake_minimum_required(VERSION 3.21)
project(sord LANGUAGES CXX)

# When configured standalone (not via the solution root), pull in the shared
# toolchain config and the common module.
if(NOT TARGET hft::common)
  include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/HftConfig.cmake)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../common ${CMAKE_CURRENT_BINARY_DIR}/common)
endif()

# Headers listed explicitly so IDE generators show them in the project tree.
set(SR_HEADERS
  include/sr/network.hpp
  include/sr/router.hpp
  include/sr/sim.hpp
)

set(SR_DOCS README.md)

add_executable(sord src/main.cpp ${SR_HEADERS} ${SR_DOCS})
hft_configure(sord)

add_executable(sord_tests src/tests.cpp ${SR_HEADERS})
hft_configure(sord_tests)

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}
             FILES src/main.cpp src/tests.cpp ${SR_HEADERS} ${SR_DOCS})
