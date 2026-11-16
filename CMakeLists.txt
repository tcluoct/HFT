cmake_minimum_required(VERSION 3.21)

# hft — a family of standalone low-latency lab projects sharing one toolbox.
# Configuring this file produces a single solution (hft.sln under VS) with
# every project in it; each project directory can also be configured on its
# own and will pull in common/ + cmake/HftConfig.cmake itself.
project(hft LANGUAGES CXX)

include(cmake/HftConfig.cmake)

add_subdirectory(common)

# The platform ships the ENGINE and the STRATEGY SDK — no strategy — then the
# strategy-team static libraries, then the desk that composes them into the
# lineup the platform apps link. The order matters once: the apps in platform/
# link desk, and CMake resolves that reference at generate time.
add_subdirectory(platform)
add_subdirectory(strategies)
add_subdirectory(desk)

add_subdirectory(tick2trade)
add_subdirectory(matchd)
add_subdirectory(ticklab)
add_subdirectory(seqbus)
add_subdirectory(riskgate)
add_subdirectory(feedarb)
add_subdirectory(wirecodec)
add_subdirectory(sesslink)
add_subdirectory(omsd)
add_subdirectory(symtab)
add_subdirectory(sigcalc)
add_subdirectory(pnld)
add_subdirectory(sord)
add_subdirectory(quotd)

# F5 starts the daemon, not CMake's non-executable ALL_BUILD target.
set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT platd)
