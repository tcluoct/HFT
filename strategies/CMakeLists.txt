# strategies — the strategy-team static_library projects.
#
# One directory per team (trend, fade, basis, cash), each an independent CMake
# project with its own include tree, static lib, tests and README. A team may
# own one strategy type or a family that shares signal machinery.
#
# THE DEPENDENCY DIRECTION IS THE POINT:
#
#     platform (engine + strategy SDK)
#         ▲
#         │  links strat_sdk only — never the engine
#     strategies/<team>   (static libs, this tree)
#         ▲
#         │  links the teams it fields
#     desk                (the deployed lineup: Pack + PackSite)
#         ▲
#         │
#     platd / platbt / platops / plat_bench / plat_tests
#
# The platform engine never includes a strategy header; a book links a desk.
# Add the directory here and the ones under it — the root CMakeLists adds this
# tree right after the platform libraries.

add_subdirectory(trend)
add_subdirectory(fade)
add_subdirectory(basis)
add_subdirectory(cash)
