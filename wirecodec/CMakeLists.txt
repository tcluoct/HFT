cmake_minimum_required(VERSION 3.21)
project(wirecodec LANGUAGES CXX)

# When configured standalone (not via the solution root), pull in the shared
# toolchain config and the common module.
if(NOT TARGET hft::common)
  include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/HftConfig.cmake)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../common ${CMAKE_CURRENT_BINARY_DIR}/common)
endif()

# Headers listed explicitly so IDE generators show them in the project tree.
set(WC_HEADERS
  include/wc/core/fixed_string.hpp
  include/wc/core/codec.hpp
  include/wc/core/frame.hpp
  include/wc/proto/orders.hpp
)

set(WC_DOCS README.md)

add_executable(wirecodec src/main.cpp ${WC_HEADERS} ${WC_DOCS})
hft_configure(wirecodec)

add_executable(wirecodec_tests src/tests.cpp ${WC_HEADERS})
hft_configure(wirecodec_tests)

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}
             FILES src/main.cpp src/tests.cpp ${WC_HEADERS} ${WC_DOCS})
