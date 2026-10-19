cmake_minimum_required(VERSION 3.20)
project(DotRPC C)

# 32-bit Windows only: PS2 RAM lives at its console addresses.
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "DotR-PC builds 32-bit only: run build.cmd (vcvars32 + clang-cl)")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_compile_options(/W3 -Wno-unused-command-line-argument)
add_compile_definitions(_CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX)
include_directories(${CMAKE_SOURCE_DIR}/hw)

# developer switches (--debug-menu); the release package is built without them (build.cmd Package)
option(DOTR_DEV "developer switches" ON)
if(DOTR_DEV)
  add_compile_definitions(DOTR_DEV)
endif()
# the vanilla edition: the PS2 game as it was, nothing added (a difference from the PS2 is a bug); OFF only for the upgrades
# edition. Always defined as 0 or 1: code tests it with #if DOTR_VANILLA / #if !DOTR_VANILLA.
option(DOTR_VANILLA "the PS2 game as it was: no fast-forward, no texture replacement" ON)
if(DOTR_VANILLA)
  add_compile_definitions(DOTR_VANILLA=1)
else()
  add_compile_definitions(DOTR_VANILLA=0)
endif()

enable_testing()

foreach(dir loader runtime gfx sound movie mcimport recomp tests)
  if(EXISTS ${CMAKE_SOURCE_DIR}/${dir}/CMakeLists.txt)
    add_subdirectory(${dir})
  endif()
endforeach()
