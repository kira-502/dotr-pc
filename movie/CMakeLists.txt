# Lane G: movies. The disc's PSS files are demuxed here and decoded by Windows' own MPEG-2 decoder.
add_library(movie STATIC pss.c mpeg2_mf.c movie_run.c)
target_include_directories(movie PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(movie PUBLIC mfplat mfuuid ole32)

# Headless check on the owner's disc: movdump <game.iso> <MOVIE/OPMV_US.PSS> <out-dir> [prefix]
add_executable(movdump tools/movdump.c ${CMAKE_SOURCE_DIR}/sound/tools/iso9660.c)
target_link_libraries(movdump movie)

# The game-facing replacement of movie_play (needs the runtime's disc reader and the game's pad/vsync functions):
# lane P links this into the game.
add_library(movie_game STATIC movie_play.c)
target_link_libraries(movie_game PUBLIC movie gfx snd)

# Headless check of the in-game loop with a simulated clock/pad/sound: movsim <game.iso> <MOVIE/x.PSS> [skip-at-field]
add_executable(movsim tools/movsim.c ${CMAKE_SOURCE_DIR}/sound/tools/iso9660.c)
target_link_libraries(movsim movie)
