set(GEN_OUT ${CMAKE_SOURCE_DIR}/gen/out)
file(GLOB RUNTIME_SRC CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.c)
list(REMOVE_ITEM RUNTIME_SRC ${CMAKE_CURRENT_SOURCE_DIR}/functab_empty.c ${CMAKE_CURRENT_SOURCE_DIR}/snd_stub.c
     ${CMAKE_CURRENT_SOURCE_DIR}/rc_stub.c)
# lane C's sound module when it is in the tree, otherwise a silent stand-in (hw/sif.h)
list(APPEND RUNTIME_SRC $<$<NOT:$<TARGET_EXISTS:snd>>:${CMAKE_CURRENT_SOURCE_DIR}/snd_stub.c>)
# lane R's recompiled modules when they are built, otherwise a stand-in (recomp/rc_port.h)
list(APPEND RUNTIME_SRC $<$<NOT:$<TARGET_EXISTS:rc_modules>>:${CMAKE_CURRENT_SOURCE_DIR}/rc_stub.c>)

if(EXISTS ${GEN_OUT}/CMakeLists.txt)
  # the port generator's output: game C, libc C, HLE layer (hle/), stubs, function table, data symbols
  add_subdirectory(${GEN_OUT} ${CMAKE_BINARY_DIR}/gen)
else()
  list(APPEND RUNTIME_SRC ${CMAKE_CURRENT_SOURCE_DIR}/functab_empty.c)
  add_compile_definitions(DOTR_NO_GAME)
endif()

add_library(dotr SHARED ${RUNTIME_SRC})
target_include_directories(dotr PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/recomp)
target_link_options(dotr PRIVATE /LARGEADDRESSAWARE /SAFESEH:NO)
# a fixed load address: a call to an absolute symbol (a function named by its PS2 address) is only right when
# the DLL is not relocated
target_link_options(dotr PRIVATE /BASE:0x60000000 /DYNAMICBASE:NO)
if(DOTR_GAME_LIB)
  add_dependencies(dotr dotr_game)
  target_link_options(dotr PRIVATE /WHOLEARCHIVE:${DOTR_GAME_LIB})
  target_link_libraries(dotr PRIVATE ${DOTR_GAME_LIB})
endif()
# lane G: graphics core, and the VU0 maths when the decomp snapshot is there
target_link_libraries(dotr PRIVATE gfx $<TARGET_NAME_IF_EXISTS:gfx_vu0> $<TARGET_NAME_IF_EXISTS:snd>
                      $<TARGET_NAME_IF_EXISTS:rc_modules> kernel32 user32 comdlg32 winmm)
# lane C's movie player: replaces the game's movie_play (gen/rules.json "external"); needs hw_disc_read_file
if(DOTR_GAME_LIB)
  target_link_libraries(dotr PRIVATE $<TARGET_NAME_IF_EXISTS:movie_game>)
endif()
# debug information for the crash reporter (exact function and line in a crash report)
target_compile_options(dotr PRIVATE /Zi)
target_link_options(dotr PRIVATE /DEBUG /PDBALTPATH:%_PDB%)
