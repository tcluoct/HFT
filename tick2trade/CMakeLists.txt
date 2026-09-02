cmake_minimum_required(VERSION 3.21)
project(tick2trade LANGUAGES CXX)

# When configured standalone (not via the solution root), pull in the shared
# toolchain config and the common module.
if(NOT TARGET hft::common)
  include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/HftConfig.cmake)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../common ${CMAKE_CURRENT_BINARY_DIR}/common)
endif()

# Headers are listed explicitly so IDE generators (Visual Studio, Xcode) show
# them in the project tree; they are not compiled separately.
set(T2T_HEADERS
  include/t2t/config.hpp
  include/t2t/md/feed_handler.hpp
  include/t2t/md/order_book.hpp
  include/t2t/oe/order_gateway.hpp
  include/t2t/proto/wire.hpp
  include/t2t/sim/exchange_sim.hpp
  include/t2t/strategy/imbalance_taker.hpp
)

set(T2T_DOCS
  README.md
  docs/DESIGN.md
)

add_executable(tick2trade src/main.cpp ${T2T_HEADERS} ${T2T_DOCS})
hft_configure(tick2trade)

# Mirror the on-disk layout in Solution Explorer (instead of the default flat
# "Header Files" filter).
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}
             FILES src/main.cpp ${T2T_HEADERS} ${T2T_DOCS})
