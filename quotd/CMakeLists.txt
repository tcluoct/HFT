cmake_minimum_required(VERSION 3.21)
project(quotd LANGUAGES CXX)

# When configured standalone (not via the solution root), pull in the shared
# toolchain config and the common module.
if(NOT TARGET hft::common)
  include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/HftConfig.cmake)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../common ${CMAKE_CURRENT_BINARY_DIR}/common)
endif()

# Headers listed explicitly so IDE generators show them in the project tree.
set(QT_HEADERS
  include/qt/engine.hpp
  include/qt/sim.hpp
)

set(QT_DOCS README.md)

add_executable(quotd src/main.cpp ${QT_HEADERS} ${QT_DOCS})
hft_configure(quotd)

add_executable(quotd_tests src/tests.cpp ${QT_HEADERS})
hft_configure(quotd_tests)

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}
             FILES src/main.cpp src/tests.cpp ${QT_HEADERS} ${QT_DOCS})
