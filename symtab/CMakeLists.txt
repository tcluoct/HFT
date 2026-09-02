cmake_minimum_required(VERSION 3.21)
project(symtab LANGUAGES CXX)

# When configured standalone (not via the solution root), pull in the shared
# toolchain config and the common module.
if(NOT TARGET hft::common)
  include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/HftConfig.cmake)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../common ${CMAKE_CURRENT_BINARY_DIR}/common)
endif()

# Headers listed explicitly so IDE generators show them in the project tree.
set(ST_HEADERS
  include/st/sym.hpp
  include/st/mph.hpp
  include/st/oa_map.hpp
  include/st/table.hpp
  include/st/gen.hpp
)

set(ST_DOCS README.md)

add_executable(symtab src/main.cpp ${ST_HEADERS} ${ST_DOCS})
hft_configure(symtab)

add_executable(symtab_tests src/tests.cpp ${ST_HEADERS})
hft_configure(symtab_tests)

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}
             FILES src/main.cpp src/tests.cpp ${ST_HEADERS} ${ST_DOCS})
