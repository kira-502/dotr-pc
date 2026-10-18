enable_language(RC)
# The icon (both exes): -DDOTR_ICON=..., else the DOTR_ICON environment variable, else a private icon kept next to the
# repository (../DotR-PC-private/dotr.ico, never committed), else loader/dotr.ico. Looked up on every configure.
if(NOT DEFINED DOTR_ICON)
  set(DOTR_ICON "$ENV{DOTR_ICON}")
  if(NOT DOTR_ICON AND EXISTS "${CMAKE_SOURCE_DIR}/../DotR-PC-private/dotr.ico")
    set(DOTR_ICON "${CMAKE_SOURCE_DIR}/../DotR-PC-private/dotr.ico")
  endif()
  if(NOT DOTR_ICON)
    set(DOTR_ICON "${CMAKE_CURRENT_SOURCE_DIR}/dotr.ico")
  endif()
endif()
file(TO_CMAKE_PATH "${DOTR_ICON}" DOTR_ICON_RC)
message(STATUS "Program icon: ${DOTR_ICON_RC}")
set(DOTR_RC ${CMAKE_CURRENT_BINARY_DIR}/dotr_gui.rc)
configure_file(dotr_gui.rc.in ${DOTR_RC} @ONLY)
set_source_files_properties(${DOTR_RC} PROPERTIES OBJECT_DEPENDS "${DOTR_ICON_RC}")

add_executable(DotR loader.c ${DOTR_RC})
target_compile_options(DotR PRIVATE /GS- /Gs999999 -fno-builtin)
set_target_properties(DotR PROPERTIES OUTPUT_NAME DotR)
# Fixed low base, no ASLR, no C runtime: our code runs before anything else claims low memory.
target_link_options(DotR PRIVATE /BASE:0x10000 /FIXED /DYNAMICBASE:NO /LARGEADDRESSAWARE
  /NODEFAULTLIB /ENTRY:rawMain /SUBSYSTEM:CONSOLE /STACK:65536,65536)
target_link_libraries(DotR PRIVATE kernel32)

# the window build for the release: same loader, no console window, the game icon, problems in message boxes
add_executable(DotR_GUI WIN32 loader.c ${DOTR_RC})
target_compile_definitions(DotR_GUI PRIVATE DOTR_GUI)
target_compile_options(DotR_GUI PRIVATE /GS- /Gs999999 -fno-builtin)
target_link_options(DotR_GUI PRIVATE /BASE:0x10000 /FIXED /DYNAMICBASE:NO /LARGEADDRESSAWARE
  /NODEFAULTLIB /ENTRY:rawMain /SUBSYSTEM:WINDOWS /STACK:65536,65536)
target_link_libraries(DotR_GUI PRIVATE kernel32 user32)
