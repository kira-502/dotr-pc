# Lane R: the module recompiler. rcgen.py translates; the tests are windowless.
find_package(Python3 REQUIRED COMPONENTS Interpreter)

# The EE does a multiply and an add as two roundings: never fuse them.
set(RC_FLAGS /clang:-ffp-contract=off /clang:-fwrapv /clang:-fno-strict-aliasing)

# R1: instruction tests (interpreter vs translation).
set(RC_UNIT_GEN ${CMAKE_CURRENT_BINARY_DIR}/rc_unit_gen.c)
add_custom_command(
  OUTPUT ${RC_UNIT_GEN}
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/rctest_gen.py ${RC_UNIT_GEN}
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/rctest_gen.py ${CMAKE_CURRENT_SOURCE_DIR}/rcgen.py
  COMMENT "rctest_gen: instruction tests")
add_executable(rc_unit rc_unit.c rc_interp.c ${RC_UNIT_GEN})
target_include_directories(rc_unit PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(rc_unit PRIVATE ${RC_FLAGS} /FI${CMAKE_CURRENT_SOURCE_DIR}/rc_test.h)
add_test(NAME rc_unit COMMAND rc_unit)
# The same test as a dotr.dll for the port's loader (rc_testhost.c: Smart App Control blocks new test .exe files).
add_library(rc_unit_host SHARED rc_testhost.c rc_unit.c rc_interp.c ${RC_UNIT_GEN})
target_include_directories(rc_unit_host PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(rc_unit_host PRIVATE ${RC_FLAGS} /FI${CMAKE_CURRENT_SOURCE_DIR}/rc_test.h)
target_compile_definitions(rc_unit_host PRIVATE main=rc_host_main)
set_target_properties(rc_unit_host PROPERTIES OUTPUT_NAME dotr RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/rctest/unit
                      ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/rctest/unit)

# R2: every module, translated at build time from recomp/input (gitignored, see README.md) into the build tree.
# RC_INPUT: the module asm copy. Default: this tree's recomp/input, else lane R's worktree next to this repo.
if(NOT RC_INPUT)
  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/input/func)
    set(RC_INPUT ${CMAKE_CURRENT_SOURCE_DIR}/input)
  elseif(EXISTS ${CMAKE_SOURCE_DIR}/../DotR-PC-recomp/recomp/input/func)
    get_filename_component(RC_INPUT ${CMAKE_SOURCE_DIR}/../DotR-PC-recomp/recomp/input ABSOLUTE)
  endif()
endif()
if(RC_INPUT AND EXISTS ${RC_INPUT}/func)
  message(STATUS "recomp: module input ${RC_INPUT}")
  file(GLOB RC_MOD_DIRS RELATIVE ${RC_INPUT}/func ${RC_INPUT}/func/*)
  set(RC_OUT ${CMAKE_CURRENT_BINARY_DIR}/out)
  set(RC_MOD_SRCS ${RC_OUT}/rc_modules.c)
  foreach(m ${RC_MOD_DIRS})
    list(APPEND RC_MOD_SRCS ${RC_OUT}/mod_${m}.c)
  endforeach()
  add_custom_command(
    OUTPUT ${RC_MOD_SRCS}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/rcgen.py all ${RC_OUT} ${RC_INPUT}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/rcgen.py ${RC_INPUT}/COMMIT
    COMMENT "rcgen: translating every module")
  # R4: the port build adds the thunk tables (from the port generator's gen/out, lane P) and rc_port.c.
  # Without gen/out (this lane's own tree) rc_modules is the translated modules alone, for the compile check.
  set(RC_PORT_SRCS)
  set(RC_GEN_OUT ${CMAKE_SOURCE_DIR}/gen/out)
  if(EXISTS ${RC_GEN_OUT}/include/functions.h AND EXISTS ${RC_GEN_OUT}/src/zz_functab.c)
    set(RC_PORT_SRCS ${RC_OUT}/rc_exits.c ${RC_OUT}/rc_entries.c ${CMAKE_CURRENT_SOURCE_DIR}/rc_port.c
                     ${CMAKE_CURRENT_SOURCE_DIR}/rc_check.c ${CMAKE_CURRENT_SOURCE_DIR}/rc_oracle.c
                     ${CMAKE_CURRENT_SOURCE_DIR}/rc_interp_port.c)
    add_custom_command(
      OUTPUT ${RC_OUT}/rc_exits.c ${RC_OUT}/rc_entries.c ${RC_OUT}/rc_thunks.txt
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/rcthunks.py ${RC_GEN_OUT} ${RC_OUT} ${RC_INPUT}
      DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/rcthunks.py ${CMAKE_CURRENT_SOURCE_DIR}/rcgen.py
              ${RC_GEN_OUT}/include/functions.h ${RC_GEN_OUT}/src/zz_functab.c ${RC_INPUT}/COMMIT
      COMMENT "rcthunks: main<->module thunk tables")
  endif()
  add_library(rc_modules STATIC ${RC_MOD_SRCS} ${RC_PORT_SRCS})
  target_include_directories(rc_modules PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_options(rc_modules PRIVATE ${RC_FLAGS} /W3 -Werror)
  if(RC_PORT_SRCS)
    target_compile_definitions(rc_modules PRIVATE RC_PORT_BUILD)
  endif()

  # R3: the same translation built for the tests, run against the interpreter on every module function.
  add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/img/b000.bin
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/rcgen.py images ${CMAKE_CURRENT_BINARY_DIR}/img ${RC_INPUT}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/rcgen.py ${RC_INPUT}/COMMIT
    COMMENT "rcgen: module images for rc_diff")
  add_custom_target(rc_images DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/img/b000.bin)
  add_library(rc_modules_test STATIC ${RC_MOD_SRCS})
  target_compile_options(rc_modules_test PRIVATE ${RC_FLAGS} /FI${CMAKE_CURRENT_SOURCE_DIR}/rc_test.h)
  target_include_directories(rc_modules_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_executable(rc_diff rc_diff.c rc_interp.c)
  target_include_directories(rc_diff PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_options(rc_diff PRIVATE ${RC_FLAGS} /FI${CMAKE_CURRENT_SOURCE_DIR}/rc_test.h)
  target_link_libraries(rc_diff rc_modules_test)
  target_link_options(rc_diff PRIVATE /STACK:67108864)
  add_dependencies(rc_diff rc_images)
  add_test(NAME rc_diff COMMAND rc_diff ${CMAKE_CURRENT_BINARY_DIR}/img 4)
  add_library(rc_diff_host SHARED rc_testhost.c rc_diff.c rc_interp.c)
  target_include_directories(rc_diff_host PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_options(rc_diff_host PRIVATE ${RC_FLAGS} /FI${CMAKE_CURRENT_SOURCE_DIR}/rc_test.h)
  target_compile_definitions(rc_diff_host PRIVATE main=rc_host_main)
  target_link_libraries(rc_diff_host rc_modules_test)
  set_target_properties(rc_diff_host PROPERTIES OUTPUT_NAME dotr RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/rctest/diff
                        ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/rctest/diff)
  add_dependencies(rc_diff_host rc_images)
endif()
