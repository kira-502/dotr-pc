# Lane G: the graphics core (DMA, VIF1, VU1, GIF, GS software renderer, D3D11 window, capture/replay).
set(GFX_SOURCES
  gs_mem.c gs.c gs_draw.c gs_display.c gif.c vif1.c vu1.c dma.c capture.c gfx.c display_d3d11.c vu0/vu0.c gs_fast.c gs_par.c gpu/gs_gpu.c)

add_library(gfx STATIC ${GFX_SOURCES})
target_include_directories(gfx PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# The GS on the GPU (gpu/): HLSL compiled at build time with the Windows SDK's fxc into headers in the build folder.
file(GLOB DOTR_FXC_CANDIDATES "C:/Program Files (x86)/Windows Kits/10/bin/*/x64/fxc.exe")
list(SORT DOTR_FXC_CANDIDATES)
list(REVERSE DOTR_FXC_CANDIDATES)
find_program(DOTR_FXC NAMES fxc.exe HINTS ${DOTR_FXC_CANDIDATES})
if(NOT DOTR_FXC AND DOTR_FXC_CANDIDATES)
  list(GET DOTR_FXC_CANDIDATES 0 DOTR_FXC)
endif()
set(GPU_SHADER_DIR ${CMAKE_CURRENT_BINARY_DIR}/gpu)
set(GPU_SHADER_HEADERS "")
foreach(sh upload readout copy clutload)
  add_custom_command(OUTPUT ${GPU_SHADER_DIR}/${sh}_cs.h
    COMMAND ${CMAKE_COMMAND} -E make_directory ${GPU_SHADER_DIR}
    COMMAND "${DOTR_FXC}" -nologo -O3 -T cs_5_0 -E main -Vn g_${sh}_cs -Fh ${GPU_SHADER_DIR}/${sh}_cs.h
            ${CMAKE_CURRENT_SOURCE_DIR}/gpu/${sh}.hlsl
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/gpu/${sh}.hlsl ${CMAKE_CURRENT_SOURCE_DIR}/gpu/gs_mem.hlsli
    COMMENT "fxc: gpu/${sh}.hlsl")
  list(APPEND GPU_SHADER_HEADERS ${GPU_SHADER_DIR}/${sh}_cs.h)
endforeach()
add_custom_command(OUTPUT ${GPU_SHADER_DIR}/readout_tex_cs.h
  COMMAND ${CMAKE_COMMAND} -E make_directory ${GPU_SHADER_DIR}
  COMMAND "${DOTR_FXC}" -nologo -O3 -DTO_TEXTURE=1 -T cs_5_0 -E main -Vn g_readout_tex_cs -Fh ${GPU_SHADER_DIR}/readout_tex_cs.h
          ${CMAKE_CURRENT_SOURCE_DIR}/gpu/readout.hlsl
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/gpu/readout.hlsl ${CMAKE_CURRENT_SOURCE_DIR}/gpu/gs_mem.hlsli
  COMMENT "fxc: gpu/readout.hlsl (texture)")
list(APPEND GPU_SHADER_HEADERS ${GPU_SHADER_DIR}/readout_tex_cs.h)
# the draw shaders: IEEE-strict (/Gis), the exact renderer's arithmetic order kept. Two pixel shaders: ps steps
# along rows as tri_rows does (bit-identical), ps_direct evaluates each pixel directly (the default, faster).
foreach(v vs ps ps_direct)
  set(st ${v})
  set(defs "")
  if(v STREQUAL "ps_direct")
    set(st ps)
    set(defs -DCLOSED_FORM=1)
  endif()
  add_custom_command(OUTPUT ${GPU_SHADER_DIR}/draw_${v}.h
    COMMAND ${CMAKE_COMMAND} -E make_directory ${GPU_SHADER_DIR}
    COMMAND "${DOTR_FXC}" -nologo -O3 -Gis ${defs} -T ${st}_5_0 -E ${st}_main -Vn g_draw_${v} -Fh ${GPU_SHADER_DIR}/draw_${v}.h
            ${CMAKE_CURRENT_SOURCE_DIR}/gpu/draw.hlsl
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/gpu/draw.hlsl ${CMAKE_CURRENT_SOURCE_DIR}/gpu/gs_mem.hlsli
    COMMENT "fxc: gpu/draw.hlsl (${v})")
  list(APPEND GPU_SHADER_HEADERS ${GPU_SHADER_DIR}/draw_${v}.h)
endforeach()
set_source_files_properties(gpu/gs_gpu.c PROPERTIES OBJECT_DEPENDS "${GPU_SHADER_HEADERS}")
target_include_directories(gfx PRIVATE ${GPU_SHADER_DIR})
# SSE2 float maths with no FMA contraction: the VU and GS maths must not change with the optimiser.
target_compile_options(gfx PRIVATE /arch:SSE2 /clang:-ffp-contract=off -fno-strict-aliasing /Z7)   # /Z7: line info in dotr.pdb for profiles and crash logs
target_link_libraries(gfx PUBLIC d3d11 dxgi dxguid user32 synchronization)

# VU1 programs translated to C at build time from the owner's own disc (vu1x/): the output stays in the build
# folder and is never committed. Without the disc (or Python) the game runs VU1 in the interpreter.
# The disc: -DDOTR_ISO=..., else the DOTR_ISO environment variable, else ../ISOs/<the USA disc>.iso.
set(_dotr_iso "$ENV{DOTR_ISO}")
if(NOT _dotr_iso)
  set(_dotr_iso "${CMAKE_SOURCE_DIR}/../ISOs/Yu-Gi-Oh! The Duelists of the Roses (USA).iso")
endif()
set(DOTR_ISO "${_dotr_iso}" CACHE FILEPATH "The game disc the VU1 programs are translated from at build time")
find_package(Python3 COMPONENTS Interpreter)
if(NOT EXISTS "${DOTR_ISO}")
  message(STATUS "No disc at DOTR_ISO (${DOTR_ISO}): VU1 programs run in the interpreter")
endif()
if(Python3_FOUND AND EXISTS "${DOTR_ISO}")
  set(VU1X_INC_FILE ${CMAKE_CURRENT_BINARY_DIR}/vu1x/vu1x_prog.inc)
  add_custom_command(OUTPUT ${VU1X_INC_FILE}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/vu1x/gen_vu1x.py "${DOTR_ISO}" ${VU1X_INC_FILE}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/vu1x/gen_vu1x.py ${CMAKE_CURRENT_SOURCE_DIR}/vu1x/vu1_image.py
    COMMENT "Translating the game's VU1 programs from the disc")
  set_source_files_properties(vu1.c PROPERTIES OBJECT_DEPENDS ${VU1X_INC_FILE})
  # thousands of small translated functions with fixed-size local arrays: no per-call stack cookie
  set_source_files_properties(vu1.c PROPERTIES COMPILE_OPTIONS /GS-)
  target_include_directories(gfx PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/vu1x)
  target_compile_definitions(gfx PRIVATE VU1X_INC=1)
endif()

# Unit tests (windowless). They link a stub of the runtime's hw_* functions.
add_executable(gfx_tests tests/test_main.c tests/hw_stub.c)
target_link_libraries(gfx_tests gfx)
add_test(NAME gfx_tests COMMAND gfx_tests)

# Headless capture replay: gfxreplay <capture.gcap> <out.bmp> [reference.bmp]
add_executable(gfxreplay tools/gfxreplay.c tests/hw_stub.c)
target_link_libraries(gfxreplay gfx winmm)
target_link_options(gfxreplay PRIVATE /MAP)

# VU0 maths: generated at build time by vu0/gen_vu0.py from the pinned decomp copy lane P keeps in gen/decomp
# (it needs asm/matchings/main_027,028,057,058,059, or render/ and sdk/ since the decomp's file layout of 32a9475b, and
# config/symbol_addrs.txt). A hand-made gfx/vu0/out/
# (python gfx/vu0/gen_vu0.py <decomp-root> gfx/vu0/out) is used when that copy is missing.
set(DOTR_DECOMP "${CMAKE_SOURCE_DIR}/gen/decomp" CACHE PATH "pinned decomp copy (src, config, asm/matchings)")
find_package(Python3 COMPONENTS Interpreter)
set(VU0_FUNCS "")
if(Python3_FOUND AND (EXISTS "${DOTR_DECOMP}/asm/matchings/main_059" OR EXISTS "${DOTR_DECOMP}/asm/matchings/render/rhgeom"))
  set(VU0_FUNCS ${CMAKE_CURRENT_BINARY_DIR}/vu0/vu0_funcs.c)
  add_custom_command(OUTPUT ${VU0_FUNCS}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/vu0/gen_vu0.py "${DOTR_DECOMP}" ${CMAKE_CURRENT_BINARY_DIR}/vu0
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/vu0/gen_vu0.py "${DOTR_DECOMP}/COMMIT"   # a re-pin regenerates it
    COMMENT "gen_vu0: VU0 maths from the decomp's matched assembly")
elseif(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/vu0/out/vu0_funcs.c)
  set(VU0_FUNCS ${CMAKE_CURRENT_SOURCE_DIR}/vu0/out/vu0_funcs.c)
else()
  message(WARNING "gfx: no decomp copy with asm/matchings at ${DOTR_DECOMP}; gfx_vu0 (VU0 maths) is not built")
endif()
if(VU0_FUNCS)
  add_library(gfx_vu0 STATIC ${VU0_FUNCS})
  target_include_directories(gfx_vu0 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/vu0)
  target_compile_options(gfx_vu0 PRIVATE /arch:SSE2 /clang:-ffp-contract=off -fno-strict-aliasing -Wno-unused-label -Wno-unused-function)
  target_link_libraries(gfx_vu0 PUBLIC gfx)

  # Tests of the translated functions; they stub the 14 native game functions those call.
  add_executable(gfx_vu0_tests tests/test_vu0.c tests/hw_stub.c)
  target_link_libraries(gfx_vu0_tests gfx_vu0)
  target_link_options(gfx_vu0_tests PRIVATE /BASE:0x40000000 /FIXED /DYNAMICBASE:NO)
  add_test(NAME gfx_vu0_tests COMMAND gfx_vu0_tests)
endif()

# Windowless check on the game's own executable: gfxboot <SLUS_205.15> [-d]
add_executable(gfxboot tools/gfxboot.c tests/hw_stub.c)
target_link_libraries(gfxboot gfx)


# Renderer speed on typical DotR work (windowless): gfxbench
add_executable(gfxbench tools/gfxbench.c tests/hw_stub.c)
target_link_libraries(gfxbench gfx winmm)
target_link_options(gfxbench PRIVATE /MAP)
