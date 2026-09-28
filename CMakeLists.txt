# dasm — on-device SH-4A disassembler / binary browser (fx-CG50, gint).
# Build inside the fxsdk:latest container (from PowerShell, Windows-style path):
#   docker run --rm -v "<path-to-dasm>:/work" fxsdk:latest fxsdk build-cg
cmake_minimum_required(VERSION 3.15)
project(dasm C)

include(GenerateG3A)
include(Fxconv)
find_package(Gint 2.1 REQUIRED)

set(SOURCES
  src/main.c
  src/sh4dec.c
  src/source.c
  src/names.c
)
set(ASSETS
  assets-cg/font5x7.png
)

fxconv_declare_assets(${ASSETS} WITH_METADATA)

add_executable(dasm ${SOURCES} ${ASSETS})
target_compile_options(dasm PRIVATE -Wall -Wextra -Os)
target_link_libraries(dasm Gint::Gint)

generate_g3a(TARGET dasm OUTPUT "DASM.g3a"
  NAME "Dasm" ICONS assets-cg/icon-uns.png assets-cg/icon-sel.png)
