cmake_minimum_required(VERSION 3.21)
project(matchd LANGUAGES CXX)

# When configured standalone (not via the solution root), pull in the shared
# toolchain config and the common module.
if(NOT TARGET hft::common)
  include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/HftConfig.cmake)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../common ${CMAKE_CURRENT_BINARY_DIR}/common)
endif()

# Headers listed explicitly so IDE generators show them in the project tree.
set(ME_HEADERS
  include/me/core/types.hpp
  include/me/core/order_pool.hpp
  include/me/core/book.hpp
  include/me/core/matching_engine.hpp
  include/me/proto/ouch.hpp
  include/me/journal/journal.hpp
)

set(ME_DOCS README.md)

add_executable(matchd src/main.cpp ${ME_HEADERS} ${ME_DOCS})
hft_configure(matchd)

add_executable(matchd_tests src/tests.cpp ${ME_HEADERS})
hft_configure(matchd_tests)

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}
             FILES src/main.cpp src/tests.cpp ${ME_HEADERS} ${ME_DOCS})
