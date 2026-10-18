# Lane G: PCSX2 memory card import. Copies the saves of a .ps2 card image into the port's save folders
# (Saves\cardN\DIR\FILE, the layout of runtime/memcard.c).
add_library(ps2card STATIC ps2card.c)
target_include_directories(ps2card PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# mcimport <card.ps2> <Saves\card1 folder> [--overwrite]
add_executable(mcimport tools/mcimport.c)
target_link_libraries(mcimport ps2card)

# Synthetic cards only (never the owner's real cards)
add_executable(mcimport_tests tests/test_mcimport.c)
target_link_libraries(mcimport_tests ps2card)
add_test(NAME mcimport_tests COMMAND mcimport_tests)
