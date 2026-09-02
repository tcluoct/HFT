cmake_minimum_required(VERSION 3.21)
project(ticklab LANGUAGES CXX)

# When configured standalone (not via the solution root), pull in the shared
# toolchain config and the common module.
if(NOT TARGET hft::common)
  include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/HftConfig.cmake)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../common ${CMAKE_CURRENT_BINARY_DIR}/common)
endif()

set(TL_HEADERS
  include/tl/data/tick.hpp
  include/tl/data/codec.hpp
  include/tl/data/format.hpp
  include/tl/data/mapped_file.hpp
  include/tl/data/recorder.hpp
  include/tl/data/replayer.hpp
  include/tl/sim/session_gen.hpp
  include/tl/bt/context.hpp
  include/tl/bt/strategy.hpp
)

set(TL_DOCS README.md)

add_executable(ticklab src/main.cpp ${TL_HEADERS} ${TL_DOCS})
hft_configure(ticklab)

add_executable(ticklab_tests src/tests.cpp ${TL_HEADERS})
hft_configure(ticklab_tests)

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}
             FILES src/main.cpp src/tests.cpp ${TL_HEADERS} ${TL_DOCS})
