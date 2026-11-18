cmake_minimum_required(VERSION 3.21)
project(platform LANGUAGES CXX)

# Executables are opt-out: a strategy-team or desk project that pulls this
# project in standalone wants only the libraries (plat_core + strat_sdk +
# platform_lib), not a whole desk of daemons.
option(HFT_PLATFORM_APPS "Build the platform executables (platd, platbt, platops, ...)" ON)

# Also select the daemon when this directory is configured standalone.
if(HFT_PLATFORM_APPS)
  set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT platd)
endif()

# When configured standalone (not via the solution root), pull in the shared
# toolchain config and the common module.
if(NOT TARGET hft::common)
  include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/HftConfig.cmake)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../common ${CMAKE_CURRENT_BINARY_DIR}/common)
endif()

# Build identity for the session fingerprint (engine/fingerprint.hpp): a
# journal is only resumed/replayed on the build that produced it. `git
# describe --dirty` distinguishes a locally modified tree; "dev" if git is
# unavailable (a source tarball), which still separates it from a commit id.
execute_process(
  COMMAND git describe --always --dirty
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  OUTPUT_VARIABLE HFT_BUILD_ID
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET)
if(NOT HFT_BUILD_ID)
  set(HFT_BUILD_ID "dev")
endif()
message(STATUS "platform: build id = ${HFT_BUILD_ID}")

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
# This project ships the ENGINE and the STRATEGY SDK. It ships no strategy:
# the deployed lineup is the desk project's (desk/pack.hpp), and the
# implementations are the strategy-team libraries under strategies/.
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

# STRAT_SDK_HEADERS is the STRATEGY SDK — everything a strategy-team library
# may include: normalized market-data/reference/accounting structs, the
# books, and the plat/sdk contract + primitives. It is its own static library
# (strat_sdk) so the dependency arrow is explicit:
#
#     plat_core → strat_sdk → strategies/<team>
#                     ↑
#                     └──── platform_lib (engine) → desk → apps
#
# A team library links strat_sdk and never the engine; the engine meets
# strategies only through plat/sdk/framework.hpp.
set(STRAT_SDK_HEADERS
  include/plat/codec/msgs.hpp
  include/plat/ref/instrument.hpp
  include/plat/ref/fees.hpp
  include/plat/md/stock_book.hpp
  include/plat/md/fut_book.hpp
  include/plat/pnl/pnl.hpp
  include/plat/sdk/framework.hpp
  include/plat/sdk/signals.hpp
  include/plat/sdk/kbar.hpp
  include/plat/sdk/indicators.hpp
  include/plat/sdk/cycle.hpp
)

# PLAT_HEADERS is the SHARED ENGINE — transport/journal, books, OMS/PnL/risk,
# the site and the pack registry. NO concrete strategy lives here; the lineup
# is the desk project's, composed in desk/pack.hpp. Headers that belong to a
# single binary's role are listed with that binary instead
# (PLATD_/PLATBT_/PLATREC_HEADERS).
set(PLAT_HEADERS
  include/plat/bus/envelope.hpp
  include/plat/bus/ring.hpp
  include/plat/bus/trade_ring.hpp
  include/plat/bus/shared_channel.hpp
  include/plat/bus/stream.hpp
  include/plat/bus/journal.hpp
  include/plat/bus/async_journal.hpp
  include/plat/md/feed.hpp
  include/plat/oms/oms.hpp
  include/plat/risk/dup_breaker.hpp
  include/plat/risk/kill_switch.hpp
  include/plat/risk/limits.hpp
  include/plat/risk/riskgate.hpp
  include/plat/risk/session_guard.hpp
  include/plat/risk/stock_rules.hpp
  include/plat/comp/compliance.hpp
  include/plat/gw/gateway.hpp
  include/plat/engine/site.hpp
  include/plat/engine/account.hpp
  include/plat/engine/site_dispatch.hpp
  include/plat/engine/site_state.hpp
  include/plat/engine/pack.hpp
  include/plat/engine/params.hpp
  include/plat/engine/fingerprint.hpp
  include/plat/engine/snapshot.hpp
  include/plat/testing/harness.hpp
  include/plat/tel/telemetry.hpp
  include/plat/tel/latency.hpp
)

# platd-only: the live venue (CTP client), the live driver, replay/recovery
# tooling, and per-strategy position carry.
set(PLATD_HEADERS
  include/plat/ctp/md_client.hpp
  include/plat/ctp/trader_client.hpp
  include/plat/ctp/trader_client_orders.hpp
  include/plat/ctp/trader_client_queries.hpp
  include/plat/engine/live.hpp
  include/plat/engine/reconcile.hpp
  include/plat/engine/session_marker.hpp
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
  src/platd_workers.cpp
  src/platd_workers.hpp
  src/platd_positions.cpp
  src/platd_fleet.cpp
  src/platd_tools.cpp
)

# Docs + example configs, shown in Visual Studio under the project each one
# belongs to (the config lands beside its exe via a POST_BUILD copy too).
set(PLATD_DOCS README.md DEPLOYMENT.md MDR_FORMAT.md
               platd.example.cfg platd.fleet.example.cfg trade_calendar.example.txt)
set(PLATBT_DOCS backtest.example.cfg)
set(PLATREC_DOCS platrec.example.cfg)
set(ACCREP_DOCS accrep.example.cfg)

# ---------------------------------------------------------------- CTP SDK
# Official SFIT CTP client libraries (see DEPLOYMENT.md for provenance and
# version pinning). Headers ship UTF-8; import libs are win64 / linux64.
#   PLAT_CTP_SDK=prod  third_party/ctp     v6.7.13 API (default; Windows x64)
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

# strat_sdk — the strategy SDK: everything a strategy-team library may
# include, and nothing that knows about orders, risk or the site. Static
# (header-only + one anchor TU) so a team links exactly one target.
add_library(strat_sdk STATIC src/strat_sdk.cpp ${STRAT_SDK_HEADERS})
target_include_directories(strat_sdk PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(strat_sdk PUBLIC plat_core)
set_target_properties(strat_sdk PROPERTIES FOLDER ${PROJECT_NAME})
if(MSVC)
  target_compile_options(strat_sdk PRIVATE /utf-8 /EHsc)
endif()
if(HFT_IPO_OK)  # match the executables' Release LTCG so the anchor obj links clean
  set_property(TARGET strat_sdk PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
endif()

# platform_lib — the header-only trading engine, gathered into ONE target.
# The site, books, OMS/PnL/risk and CTP adapters are all header-only; this
# library owns those headers (compiling only the tiny anchor TU) and layers
# on strat_sdk. Every engine binary below links it and REUSES these headers —
# none re-lists them, so the platform headers (bus/, oms/, engine/, …) appear
# in exactly one project and are never duplicated across platd / platbt /
# plat_tests / plat_bench. It contains NO concrete strategy: binaries that
# need a lineup link the desk on top.
add_library(platform_lib STATIC src/plat_lib.cpp ${PLAT_HEADERS})
target_link_libraries(platform_lib PUBLIC strat_sdk)  # include paths come with it
target_include_directories(platform_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../seqbus/include)
target_compile_definitions(platform_lib PUBLIC HFT_BUILD_ID="${HFT_BUILD_ID}")
set_target_properties(platform_lib PROPERTIES FOLDER ${PROJECT_NAME})
if(MSVC)
  target_compile_options(platform_lib PRIVATE /utf-8 /EHsc)
endif()
if(HFT_IPO_OK)  # match the executables' Release LTCG so the anchor obj links clean
  set_property(TARGET platform_lib PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
endif()

if(HFT_PLATFORM_APPS)

# platd — the production site daemon (live / --fleet / --check / --replay).
# Links the desk: the deployed strategy lineup is a composition choice, not a
# platform dependency.
add_executable(platd ${PLATD_SOURCES} ${PLATD_HEADERS} ${PLATD_DOCS})
hft_configure(platd)
target_link_libraries(platd PRIVATE platform_lib desk ${CTP_LIBS})
foreach(dll ${CTP_DLLS})
  add_custom_command(TARGET platd POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${dll} $<TARGET_FILE_DIR:platd>)
endforeach()
# Example configs are copied next to the exe for a manual run from the output
# directory. NOT a POST_BUILD copy: editing a .cfg relinks nothing, so the
# target stays "up to date", the build step never runs, and the copy beside
# the exe silently keeps yesterday's contents (an expired-contract fleet.cfg
# is exactly how every shard aborts a second after launch). A custom target
# has no output to be up to date, so the sync runs on every build of platd.
add_custom_target(platd_example_configs
  COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:platd>
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_CURRENT_SOURCE_DIR}/platd.example.cfg
          ${CMAKE_CURRENT_SOURCE_DIR}/platd.fleet.example.cfg
          $<TARGET_FILE_DIR:platd>
  COMMENT "Syncing platd example configs next to the exe"
  VERBATIM)
add_dependencies(platd platd_example_configs)
# F5 experience in Visual Studio: run from the exe's own directory (where the
# CTP DLLs and the runtime's journals/logs belong), but read the config
# straight from the source tree — the file the operator edits in the IDE. That
# makes an edit take effect on the very next F5 without depending on whether
# the build system noticed the .cfg at all.
set_target_properties(platd PROPERTIES
  VS_DEBUGGER_WORKING_DIRECTORY $<TARGET_FILE_DIR:platd>
  VS_DEBUGGER_COMMAND_ARGUMENTS
    "--fleet \"${CMAKE_CURRENT_SOURCE_DIR}/platd.fleet.example.cfg\"")

# plat_tests — component tests + CTP mapping tests (no counter connection,
# no linkage against the CTP libs: only conv.hpp's typedefs are included).
# Engine-level, so it composes the desk only as the end-to-end stimulus; the
# strategy-team and desk suites are their own executables (strategies/*,
# desk/).
add_executable(plat_tests
  src/tests_main.cpp src/tests_util.hpp src/tests_components.cpp src/tests_risk.cpp src/tests_engine.cpp
  src/tests_recorder.cpp src/tests_signals.cpp src/tests_ops.cpp src/tests_safety.cpp)
hft_configure(plat_tests)
target_link_libraries(plat_tests PRIVATE desk ${CTP_LIBS})
foreach(dll ${CTP_DLLS})
  add_custom_command(TARGET plat_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${dll} $<TARGET_FILE_DIR:plat_tests>)
endforeach()

# plat_bench — hot-path budget lines through the production dispatch chain
# (synthetic input, capture gateway; no simulation, no CTP linkage). Uses the
# desk's momentum as the measured strategy.
add_executable(plat_bench src/bench_main.cpp)
hft_configure(plat_bench)
target_link_libraries(plat_bench PRIVATE desk)

# platbt — the strategy backtester: replays recorded .mdr data through the
# SAME Site + desk lineup platd runs live (no CTP linkage). Reuses the shared
# engine via platform_lib; lists only its own fill simulator + report builder.
add_executable(platbt src/platbt_main.cpp ${PLATBT_HEADERS} ${PLATBT_DOCS})
hft_configure(platbt)
target_link_libraries(platbt PRIVATE desk)
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
  include/plat/ops/auth.hpp include/plat/ops/tail.hpp include/plat/ops/http.hpp)
hft_configure(platops)
target_link_libraries(platops PRIVATE desk)
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

endif()  # HFT_PLATFORM_APPS

# Fold every listed file by its on-disk path in whichever project owns it, so
# the IDE tree mirrors include/ and src/. Example configs and docs are shown
# under their project (VS lists non-compiled files as "None" items).
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}
             FILES ${PLATD_SOURCES} src/plat_core.cpp src/plat_lib.cpp src/strat_sdk.cpp
                   src/tests_main.cpp
                   src/tests_components.cpp src/tests_risk.cpp
                   src/tests_engine.cpp src/tests_recorder.cpp src/tests_signals.cpp
                   src/tests_util.hpp src/bench_main.cpp
                   src/platrec_main.cpp src/platbt_main.cpp
                   src/accrep_main.cpp
                   ${PLAT_CORE_HEADERS} ${STRAT_SDK_HEADERS} ${PLAT_HEADERS}
                   ${PLATD_HEADERS} ${PLATBT_HEADERS}
                   ${PLATREC_HEADERS} ${ACCREP_HEADERS} ${PLATD_DOCS} ${PLATBT_DOCS}
                   ${PLATREC_DOCS} ${ACCREP_DOCS})

# Standalone configure of platform: compose its own strategy lineup so the
# apps have a desk to link. In the solution build the root adds strategies/
# and desk/ itself after this project; target references resolve forward.
if(HFT_PLATFORM_APPS AND PROJECT_IS_TOP_LEVEL)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../strategies ${CMAKE_BINARY_DIR}/strategies)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../desk ${CMAKE_BINARY_DIR}/desk)
endif()
