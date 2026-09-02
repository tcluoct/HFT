cmake_minimum_required(VERSION 3.21)
project(riskgate LANGUAGES CXX)

# When configured standalone (not via the solution root), pull in the shared
# toolchain config and the common module.
if(NOT TARGET hft::common)
  include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/HftConfig.cmake)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../common ${CMAKE_CURRENT_BINARY_DIR}/common)
endif()

# Headers listed explicitly so IDE generators show them in the project tree.
set(RG_HEADERS
  include/rg/core/types.hpp
  include/rg/core/seqlock.hpp
  include/rg/core/limits.hpp
  include/rg/core/throttle.hpp
  include/rg/core/gate.hpp
)

set(RG_DOCS README.md)

add_executable(riskgate src/main.cpp ${RG_HEADERS} ${RG_DOCS})
hft_configure(riskgate)

add_executable(riskgate_tests src/tests.cpp ${RG_HEADERS})
hft_configure(riskgate_tests)

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}
             FILES src/main.cpp src/tests.cpp ${RG_HEADERS} ${RG_DOCS})
