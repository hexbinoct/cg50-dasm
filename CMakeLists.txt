# dasm — on-device SH-4A disassembler / binary browser (fx-CG50, gint).
# Build with the fxSDK and gint:  fxsdk build-cg
cmake_minimum_required(VERSION 3.15)
project(dasm C ASM)

include(GenerateG3A)
include(Fxconv)
find_package(Gint 2.1 REQUIRED)

set(SOURCES
  src/main.c
  src/sh4dec.c
  src/source.c
  src/names.c
  src/analysis.c
  src/debug.c
  src/debug_dbh.S
  src/monitor.c
  src/monitor_blob.S
)
set(ASSETS
  assets-cg/font5x7.png
)

fxconv_declare_assets(${ASSETS} WITH_METADATA)

# The resident debugger monitor (monitor/): its own freestanding image linked at 0x8C4E0000,
# built here as monitor.bin and embedded by src/monitor_blob.S (.incbin).
set(MON_DIR ${CMAKE_CURRENT_SOURCE_DIR}/monitor)
set(MON_BIN ${CMAKE_CURRENT_BINARY_DIR}/monitor.bin)
find_program(SH_OBJCOPY sh-elf-objcopy REQUIRED)
add_custom_command(
  OUTPUT ${MON_BIN}
  COMMAND ${CMAKE_C_COMPILER} -m4-nofpu -mb -Os -ffreestanding -nostdlib -fno-builtin
    -fno-tree-loop-distribute-patterns -Wa,--dsp -Wall -Wextra
    -T ${MON_DIR}/mon.ld -Wl,--no-warn-rwx-segments -Wl,-Map=monitor.map -o monitor.elf
    ${MON_DIR}/entry.S ${MON_DIR}/mon.c ${CMAKE_CURRENT_SOURCE_DIR}/src/sh4dec.c -lgcc
  COMMAND ${SH_OBJCOPY} -O binary monitor.elf ${MON_BIN}
  DEPENDS ${MON_DIR}/entry.S ${MON_DIR}/mon.c ${MON_DIR}/mon.ld ${MON_DIR}/mon_abi.h
    src/sh4dec.c src/sh4dec.h src/font_aa.h
  WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
  COMMENT "Building the resident debugger monitor (monitor.bin)"
  VERBATIM)
set_source_files_properties(src/monitor_blob.S PROPERTIES OBJECT_DEPENDS ${MON_BIN})

add_executable(dasm ${SOURCES} ${ASSETS} ${MON_BIN})
target_compile_options(dasm PRIVATE -Wall -Wextra -Os)
target_compile_options(dasm PRIVATE $<$<COMPILE_LANGUAGE:ASM>:-Wa,-I${CMAKE_CURRENT_BINARY_DIR}>)
target_link_libraries(dasm Gint::Gint)

generate_g3a(TARGET dasm OUTPUT "DASM.g3a"
  NAME "Dasm" ICONS assets-cg/icon-uns.png assets-cg/icon-sel.png)
