cmake_minimum_required(VERSION 3.21)
project(platform LANGUAGES CXX)

# When configured standalone (not via the solution root), pull in the shared
# toolchain config and the common module.
if(NOT TARGET hft::common)
  include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/HftConfig.cmake)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../common ${CMAKE_CURRENT_BINARY_DIR}/common)
endif()

# The platform is the labs' graduation (docs/platform/ARCHITECTURE.md §20):
# header-only component libraries composed into the production site daemon.
#
# Two library layers, so every header is owned by exactly ONE project:
#
# PLAT_CORE_HEADERS is the STANDALONE-SAFE base layer (clocks, config,
# logging + crash tracing, threading, CTP conversions, the .mdr codec/file)
# — everything a tool may use without pulling in the trading engine. It
# lives in plat_core, linked by platform_lib AND by the standalone platrec.
#
# PLAT_HEADERS is the SHARED engine — books, OMS/PnL/risk, the strategy
# library, the site. It lives in platform_lib. Headers that belong to a
# single binary's role are listed with that binary instead
# (PLATD_/PLATBT_/PLATREC_HEADERS).
set(PLAT_CORE_HEADERS
  include/plat/core/types.hpp
  include/plat/core/time.hpp
  include/plat/core/phase.hpp
  include/plat/core/hash.hpp
  include/plat/core/config.hpp
  include/plat/core/log.hpp
  include/plat/core/crash.hpp
  include/plat/core/thread.hpp
  include/plat/ctp/conv.hpp
  include/plat/rec/tick_codec.hpp
  include/plat/rec/tick_file.hpp
)

set(PLAT_HEADERS
  include/plat/bus/envelope.hpp
  include/plat/bus/ring.hpp
  include/plat/bus/stream.hpp
  include/plat/bus/journal.hpp
  include/plat/bus/async_journal.hpp
  include/plat/codec/msgs.hpp
  include/plat/ref/instrument.hpp
  include/plat/ref/fees.hpp
  include/plat/md/stock_book.hpp
  include/plat/md/fut_book.hpp
  include/plat/md/feed.hpp
  include/plat/oms/oms.hpp
  include/plat/pnl/pnl.hpp
  include/plat/risk/dup_breaker.hpp
  include/plat/risk/kill_switch.hpp
  include/plat/risk/limits.hpp
  include/plat/risk/riskgate.hpp
  include/plat/risk/session_guard.hpp
  include/plat/risk/stock_rules.hpp
  include/plat/comp/compliance.hpp
  include/plat/gw/gateway.hpp
  include/plat/strat/framework.hpp
  include/plat/strat/signals.hpp
  include/plat/strat/kbar.hpp
  include/plat/strat/indicators.hpp
  include/plat/strat/cycle.hpp
  include/plat/strat/t0alpha.hpp
  include/plat/strat/momentum.hpp
  include/plat/strat/meanrev.hpp
  include/plat/strat/breakout.hpp
  include/plat/strat/basisarb.hpp
  include/plat/engine/site.hpp
  include/plat/engine/site_dispatch.hpp
  include/plat/engine/site_state.hpp
  include/plat/engine/pack.hpp
  include/plat/tel/telemetry.hpp
)

# platd-only: the live venue (CTP client), the live driver, replay/recovery
# tooling, and per-strategy position carry.
set(PLATD_HEADERS
  include/plat/ctp/md_client.hpp
  include/plat/ctp/trader_client.hpp
  include/plat/ctp/trader_client_orders.hpp
  include/plat/ctp/trader_client_queries.hpp
  include/plat/engine/live.hpp
  include/plat/engine/report.hpp
  include/plat/replay/replayer.hpp
  include/plat/replay/sidecar.hpp
  include/plat/replay/journal_dump.hpp
  include/plat/ref/positions_file.hpp
)

# platbt-only: the fill-simulator gateway and the HTML/console report builder.
set(PLATBT_HEADERS
  include/plat/rec/backtest_gateway.hpp
  include/plat/rec/backtest_report.hpp
)

# platrec-only: whole-market instrument enumeration and the cross-file merge.
# (Standalone tool — everything else it needs comes from plat_core, so it
# still links NO engine code.)
set(PLATREC_HEADERS
  include/plat/rec/inst_enum.hpp
  include/plat/rec/tick_merge.hpp
)

# accrep-only: local-network discovery, MD5, and the .docx field-filler for
# the CSRC/exchange program-trading "external system access report". No
# engine code, no CTP linkage — this tool never talks to the counter.
set(ACCREP_HEADERS
  include/plat/rep/md5.hpp
  include/plat/rep/win_paths.hpp
  include/plat/rep/winproc.hpp
  include/plat/rep/sysinfo.hpp
  include/plat/rep/docx_fill.hpp
  include/plat/rep/shot_render.hpp
)

set(PLATD_SOURCES
  src/platd_main.cpp
  src/platd_app.hpp
  src/platd_config.cpp
  src/platd_live.cpp
  src/platd_positions.cpp
  src/platd_fleet.cpp
  src/platd_tools.cpp
)

# Docs + example configs, shown in Visual Studio under the project each one
# belongs to (the config lands beside its exe via a POST_BUILD copy too).
set(PLATD_DOCS README.md DEPLOYMENT.md MDR_FORMAT.md
               platd.example.cfg platd.fleet.example.cfg)
set(PLATBT_DOCS backtest.example.cfg)
set(PLATREC_DOCS platrec.example.cfg)
set(ACCREP_DOCS accrep.example.cfg)

# ---------------------------------------------------------------- CTP SDK
# Official SFIT CTP client libraries (see DEPLOYMENT.md for provenance and
# version pinning). Headers ship UTF-8; import libs are win64 / linux64.
#   PLAT_CTP_SDK=prod  third_party/ctp     v6.7.11 production API (default)
#   PLAT_CTP_SDK=cp    third_party/ctp_cp  v6.7.7_CP 评测版 API — required by the
#                      期货评测环境 fronts (:53205/:53213); a production API
#                      there fails the handshake with 4040 "decode err".
set(PLAT_CTP_SDK prod CACHE STRING "CTP SDK flavor: prod or cp (评测版)")
set_property(CACHE PLAT_CTP_SDK PROPERTY STRINGS prod cp)
if(PLAT_CTP_SDK STREQUAL "cp")
  set(CTP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/ctp_cp)
else()
  set(CTP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/ctp)
endif()
message(STATUS "platform: CTP SDK = ${PLAT_CTP_SDK} (${CTP_DIR})")
if(WIN32)
  set(CTP_LIBS
    ${CTP_DIR}/lib/thosttraderapi_se.lib
    ${CTP_DIR}/lib/thostmduserapi_se.lib)
  set(CTP_DLLS
    ${CTP_DIR}/bin/thosttraderapi_se.dll
    ${CTP_DIR}/bin/thostmduserapi_se.dll)
else()
  set(CTP_LIBS
    ${CTP_DIR}/lib/libthosttraderapi_se.so
    ${CTP_DIR}/lib/libthostmduserapi_se.so)
  set(CTP_DLLS "")
endif()

# --------------------------------------------------------------- binaries

# plat_core — the standalone-safe base layer, gathered into ONE target that
# owns PLAT_CORE_HEADERS (compiling only a tiny anchor TU) and exports the
# include paths. platform_lib layers the engine on top; platrec links ONLY
# this, which keeps the recorder free of the trading stack without
# re-listing the shared headers in its own project.
add_library(plat_core STATIC src/plat_core.cpp ${PLAT_CORE_HEADERS})
target_include_directories(plat_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(plat_core SYSTEM PUBLIC ${CTP_DIR}/include)  # conv.hpp typedefs
target_link_libraries(plat_core PUBLIC hft::common)
set_target_properties(plat_core PROPERTIES FOLDER ${PROJECT_NAME})
if(MSVC)
  target_compile_options(plat_core PRIVATE /utf-8 /EHsc)
endif()
if(HFT_IPO_OK)  # match the executables' Release LTCG so the anchor obj links clean
  set_property(TARGET plat_core PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
endif()

# platform_lib — the header-only trading engine, gathered into ONE target.
# The engine, strategies, books, OMS/PnL/risk and CTP adapters are all
# header-only; this library owns those headers (compiling only the tiny
# anchor TU) and layers on plat_core. Every engine binary below links it and
# REUSES these headers — none re-lists them, so the platform headers (bus/,
# oms/, strat/, …) appear in exactly one project and are never duplicated
# across platd / platbt / plat_tests / plat_bench.
add_library(platform_lib STATIC src/plat_lib.cpp ${PLAT_HEADERS} src/strat_params.hpp)
target_link_libraries(platform_lib PUBLIC plat_core)  # include paths come with it
set_target_properties(platform_lib PROPERTIES FOLDER ${PROJECT_NAME})
if(MSVC)
  target_compile_options(platform_lib PRIVATE /utf-8 /EHsc)
endif()
if(HFT_IPO_OK)  # match the executables' Release LTCG so the anchor obj links clean
  set_property(TARGET platform_lib PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
endif()

# platd — the production site daemon (live / --fleet / --check / --replay).
add_executable(platd ${PLATD_SOURCES} ${PLATD_HEADERS} ${PLATD_DOCS})
hft_configure(platd)
target_link_libraries(platd PRIVATE platform_lib ${CTP_LIBS})
foreach(dll ${CTP_DLLS})
  add_custom_command(TARGET platd POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${dll} $<TARGET_FILE_DIR:platd>)
endforeach()
# Example configs land next to the exe so relative names resolve when run
# from the output dir — including under the VS debugger, whose working
# directory is set below.
foreach(cfg platd.example.cfg platd.fleet.example.cfg)
  add_custom_command(TARGET platd POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/${cfg} $<TARGET_FILE_DIR:platd>)
endforeach()
# F5 experience in Visual Studio: run from the exe's own directory (where
# the CTP DLLs and configs are), with a sensible default argument line.
set_target_properties(platd PROPERTIES
  VS_DEBUGGER_WORKING_DIRECTORY $<TARGET_FILE_DIR:platd>
  VS_DEBUGGER_COMMAND_ARGUMENTS "--fleet platd.fleet.example.cfg")

# plat_tests — component tests + CTP mapping tests (no counter connection,
# no linkage against the CTP libs: only conv.hpp's typedefs are included).
add_executable(plat_tests
  src/tests_main.cpp src/tests_util.hpp src/tests_components.cpp src/tests_risk.cpp src/tests_engine.cpp
  src/tests_recorder.cpp src/tests_signals.cpp src/tests_strats.cpp src/tests_ops.cpp)
hft_configure(plat_tests)
target_link_libraries(plat_tests PRIVATE platform_lib)

# plat_bench — hot-path budget lines through the production dispatch chain
# (synthetic input, capture gateway; no simulation, no CTP linkage).
add_executable(plat_bench src/bench_main.cpp)
hft_configure(plat_bench)
target_link_libraries(plat_bench PRIVATE platform_lib)

# platbt — the strategy backtester: replays recorded .mdr data through the
# SAME Site + strategies platd runs live (no CTP linkage). Reuses the shared
# engine via platform_lib; lists only its own fill simulator + report builder.
add_executable(platbt src/platbt_main.cpp ${PLATBT_HEADERS} ${PLATBT_DOCS})
hft_configure(platbt)
target_link_libraries(platbt PRIVATE platform_lib)
add_custom_command(TARGET platbt POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_CURRENT_SOURCE_DIR}/backtest.example.cfg $<TARGET_FILE_DIR:platbt>)

# platops — the web ops console: tails every shard's journal into a
# Replay-mode mirror Site (PnL / positions / orders / risk, ~ms behind live)
# and drives the shard cmd files from a browser. No CTP linkage, no contact
# with the trading processes.
add_executable(platops
  src/platops_main.cpp src/platops_app.hpp src/platops_json.hpp src/platops_web.hpp
  src/fleet_cfg.hpp
  include/plat/ops/tail.hpp include/plat/ops/http.hpp)
hft_configure(platops)
target_link_libraries(platops PRIVATE platform_lib)
if(WIN32)
  target_link_libraries(platops PRIVATE ws2_32)
endif()

# platrec — standalone market-data recorder (records the whole market to a
# compact .mdr file; --compare cross-datacenter verify; --dump). Links
# plat_core (NOT platform_lib — no strategy/order/risk code) plus the CTP
# md + trader libs.
add_executable(platrec src/platrec_main.cpp ${PLATREC_HEADERS} ${PLATREC_DOCS})
hft_configure(platrec)
target_link_libraries(platrec PRIVATE plat_core ${CTP_LIBS})
foreach(dll ${CTP_DLLS})
  add_custom_command(TARGET platrec POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${dll} $<TARGET_FILE_DIR:platrec>)
endforeach()
add_custom_command(TARGET platrec POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_CURRENT_SOURCE_DIR}/platrec.example.cfg $<TARGET_FILE_DIR:platrec>)

# accrep — fills the CSRC/exchange program-trading "external system access
# report" (.docx) from a config file: text fields + MD5 only (see
# accrep_main.cpp for what still needs a person — screenshots, checkboxes,
# signature). Windows-only (shells out to Expand-Archive/Compress-Archive to
# repack the .docx zip container — see plat/rep/winproc.hpp for why).
add_executable(accrep src/accrep_main.cpp ${ACCREP_HEADERS} ${ACCREP_DOCS})
hft_configure(accrep)
target_link_libraries(accrep PRIVATE plat_core)
if(WIN32)
  target_link_libraries(accrep PRIVATE ws2_32 iphlpapi)
endif()
add_custom_command(TARGET accrep POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_CURRENT_SOURCE_DIR}/accrep.example.cfg $<TARGET_FILE_DIR:accrep>)

# Fold every listed file by its on-disk path in whichever project owns it, so
# the IDE tree mirrors include/ and src/. Example configs and docs are shown
# under their project (VS lists non-compiled files as "None" items).
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}
             FILES ${PLATD_SOURCES} src/plat_core.cpp src/plat_lib.cpp src/tests_main.cpp
                   src/tests_components.cpp src/tests_risk.cpp
                   src/tests_engine.cpp src/tests_recorder.cpp src/tests_signals.cpp
                   src/tests_strats.cpp src/tests_util.hpp src/bench_main.cpp
                   src/platrec_main.cpp src/platbt_main.cpp src/strat_params.hpp
                   src/accrep_main.cpp
                   ${PLAT_CORE_HEADERS} ${PLAT_HEADERS} ${PLATD_HEADERS} ${PLATBT_HEADERS}
                   ${PLATREC_HEADERS} ${ACCREP_HEADERS} ${PLATD_DOCS} ${PLATBT_DOCS}
                   ${PLATREC_DOCS} ${ACCREP_DOCS})
