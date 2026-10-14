cmake_minimum_required(VERSION 3.22)
project(refract_digitalis LANGUAGES C CXX)

# Digitalis (the ARM64 -> x86_64 translator Refract uses in the emulator) built for Windows as a
# CpuCore backend of refract_native. Digitalis needs clang (GNU inline asm, naked functions), so
# this is a separate project built with clang-cl; refract_native (MSVC) links the static library.
# Only the translator core is compiled (modules.cmake): decoder, interpreter, lite translator, heavy
# optimizer, backend, assembler, translation cache. Digitalis' Linux layer (signals, threads,
# syscalls, exec memory) is replaced by src/*.cpp.

# C++23: MSVC's STL assigns std::tie(...) from std::array only in C++23 mode (libc++ does in 20).
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

set(DIGITALIS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../external/src/digitalis" CACHE PATH
    "Digitalis tree (frameworks/libs/binary_translation + gen/ with Soong's generated headers)")
set(DIGITALIS_GEN "${DIGITALIS_ROOT}/gen")
set(DIGITALIS_COMPAT "${CMAKE_CURRENT_SOURCE_DIR}/compat")

function(digitalis_module_options target)
  target_compile_definitions(${target} PRIVATE
    NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS _USE_MATH_DEFINES _HAS_EXCEPTIONS=0)
  target_compile_options(${target} PRIVATE
    /EHs-c- /GR- /Zc:__cplusplus
    /FI${DIGITALIS_COMPAT}/win_prelude.h
    # Same x86 baseline as the emulator build: Digitalis' generated code assumes SSE4.2 + POPCNT.
    -mssse3 -msse4.1 -msse4.2 -mpopcnt -mcx16
    -fno-strict-aliasing
    # Digitalis keys "64-bit host" off __LP64__ and GNU vector attributes off __GNUC__. Windows is
    # LLP64, but every such use is about pointer size or __int128, not long. Wide float math wants
    # x87 long double.
    /clang:-fgnuc-version=4.2.1 -D__LP64__=1 /clang:-mlong-double-80
    -Wno-everything)
endfunction()

include(modules.cmake)

set(PORT_SOURCES
  src/platform_win.cpp
  src/runtime_win.cpp
  src/cpu_digitalis.cpp
)

add_library(refract_digitalis STATIC ${PORT_SOURCES})
foreach(m ${DIGITALIS_MODULES})
  target_sources(refract_digitalis PRIVATE $<TARGET_OBJECTS:${m}>)
endforeach()
# The port sees the runtime's headers the way runtime/arm64 does, plus refract_native's.
target_include_directories(refract_digitalis PRIVATE ${dg_runtime_arm64_INCLUDES}
  ${dg_interpreter_arm64_INCLUDES}
  ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_definitions(refract_digitalis PRIVATE ${dg_runtime_arm64_DEFINES})
digitalis_module_options(refract_digitalis)
