cmake_minimum_required(VERSION 3.21)
project(sesslink LANGUAGES CXX)

# When configured standalone (not via the solution root), pull in the shared
# toolchain config and the common module.
if(NOT TARGET hft::common)
  include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/HftConfig.cmake)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../common ${CMAKE_CURRENT_BINARY_DIR}/common)
endif()

# Headers listed explicitly so IDE generators show them in the project tree.
set(SL_HEADERS
  include/sl/core/wire.hpp
  include/sl/core/transport.hpp
  include/sl/core/flat_hash.hpp
  include/sl/core/session.hpp
  include/sl/net/tcp.hpp
  include/sl/sim/mem_link.hpp
  include/sl/sim/apps.hpp
)

set(SL_DOCS README.md)

add_executable(sesslink src/main.cpp ${SL_HEADERS} ${SL_DOCS})
hft_configure(sesslink)
if(WIN32)
  target_link_libraries(sesslink PRIVATE ws2_32)
endif()

add_executable(sesslink_tests src/tests.cpp ${SL_HEADERS})
hft_configure(sesslink_tests)

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}
             FILES src/main.cpp src/tests.cpp ${SL_HEADERS} ${SL_DOCS})
