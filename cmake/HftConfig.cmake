# Shared build configuration for every project in the hft solution.
# Included once by the top-level CMakeLists.txt, or by an individual project's
# CMakeLists.txt when that project is configured standalone.
include_guard(GLOBAL)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Group targets into solution folders in IDE generators.
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# Link-time optimization (whole-program inlining across TUs).
include(CheckIPOSupported)
check_ipo_supported(RESULT HFT_IPO_OK OUTPUT _hft_ipo_msg)

# Apply the family-wide hot-path build contract to a target:
#   /GR-      : no RTTI (no dynamic_cast/typeid on the hot path)
#   /GS-      : no stack-security cookies in Release (measured code only)
#   /arch:AVX2: assume modern hardware, enables wider codegen
#   /wd4324   : "structure padded due to alignas" is intentional (false-sharing padding)
#   /Zi+/DEBUG: PDBs in Release too — crash post-mortems (plat/core/crash.hpp)
#               and minidumps symbolize to function + file:line; /OPT:REF,ICF
#               keeps the optimized codegen identical.
function(hft_configure tgt)
  target_link_libraries(${tgt} PRIVATE hft::common)
  target_include_directories(${tgt} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
  set_target_properties(${tgt} PROPERTIES FOLDER ${PROJECT_NAME})
  if(MSVC)
    target_compile_options(${tgt} PRIVATE
      /utf-8 /W4 /permissive- /Zc:__cplusplus /Zc:preprocessor /EHsc /GR- /wd4324 /wd5105
      $<$<CONFIG:Release>:/O2 /Ob3 /Oi /Ot /GS- /arch:AVX2 /Zi>)
    target_compile_definitions(${tgt} PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_link_options(${tgt} PRIVATE
      $<$<CONFIG:Release>:/INCREMENTAL:NO /DEBUG /OPT:REF /OPT:ICF>)
  else()
    target_compile_options(${tgt} PRIVATE -Wall -Wextra
      $<$<CONFIG:Release>:-O3 -march=native>)
    find_package(Threads REQUIRED)
    target_link_libraries(${tgt} PRIVATE Threads::Threads)
    if(UNIX AND NOT APPLE)
      target_link_libraries(${tgt} PRIVATE rt)  # shm_open lives in librt on older glibc
    endif()
  endif()
  if(HFT_IPO_OK)
    set_property(TARGET ${tgt} PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
  endif()
endfunction()
