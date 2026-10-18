# Lane G: sound. A small IOP emulator runs the game's own sound driver (KCEJEAST.IRX) on an emulated SPU2.
add_library(snd STATIC iop_cpu.c iop_irx.c iop_hle.c iop_hw.c spu2.c snd.c snd_out.c)
target_include_directories(snd PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(snd PRIVATE -fno-strict-aliasing)
target_link_libraries(snd PUBLIC xaudio2 ole32)

# Headless check on the owner's disc: sndbgm <game.iso> <SOUND/BGM/xx/yyyy.PCM> <seconds> <out.wav>
add_executable(sndbgm tools/sndbgm.c tools/iso9660.c ${CMAKE_SOURCE_DIR}/gfx/tests/hw_stub.c)
target_link_libraries(sndbgm snd)

# Sound effects check: sndse <game.iso> <SOUND/SYSTEM/SYSTEM0.GRP> <slot> <out.wav> [seconds per sound]
add_executable(sndse tools/sndse.c tools/sdee.c tools/iso9660.c ${CMAKE_SOURCE_DIR}/gfx/tests/hw_stub.c)
target_link_libraries(sndse snd)

# Unit tests (windowless)
add_executable(snd_tests tests/test_snd.c ${CMAKE_SOURCE_DIR}/gfx/tests/hw_stub.c)
target_link_libraries(snd_tests snd)
add_test(NAME snd_tests COMMAND snd_tests)
