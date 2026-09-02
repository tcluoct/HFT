# Shared header-only toolbox (namespace hft): platform primitives, TSC clock,
# latency histogram, SPSC ring, async logger, run-phase control, fenced
# bulk-benchmark helper.
set(HFT_COMMON_HEADERS
  include/hft/common/platform.hpp
  include/hft/common/tsc_clock.hpp
  include/hft/common/histogram.hpp
  include/hft/common/spsc_ring.hpp
  include/hft/common/async_logger.hpp
  include/hft/common/run_control.hpp
  include/hft/common/bench.hpp
)

add_library(hft_common INTERFACE)
add_library(hft::common ALIAS hft_common)
target_include_directories(hft_common INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)

# INTERFACE libraries have no build step; a stub target makes the shared
# headers visible in the IDE project tree.
add_custom_target(hft_common_headers SOURCES ${HFT_COMMON_HEADERS})
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${HFT_COMMON_HEADERS})
set_target_properties(hft_common_headers PROPERTIES FOLDER common)
