cmake_minimum_required(VERSION 3.21)

# hft — a family of standalone low-latency lab projects sharing one toolbox.
# Configuring this file produces a single solution (hft.sln under VS) with
# every project in it; each project directory can also be configured on its
# own and will pull in common/ + cmake/HftConfig.cmake itself.
project(hft LANGUAGES CXX)

include(cmake/HftConfig.cmake)

add_subdirectory(common)
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
add_subdirectory(platform)
