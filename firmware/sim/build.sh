#!/bin/sh
# Builds the simulator's engine (sim/sim.cpp) to web/sim.wasm with
# Emscripten (brew install emscripten). The .wasm is committed, so the
# firmware build doesn't need Emscripten: rerun this after changing the
# engine (lib/drip_core), the fonts or src/settings.cpp.
#
# Run from firmware/: sh sim/build.sh
set -e
em++ -O2 -std=c++17 -fno-exceptions -fno-rtti -Wall -Werror \
  --no-entry -sSTANDALONE_WASM -sINITIAL_MEMORY=4mb -sALLOW_MEMORY_GROWTH \
  -Isim -Ilib/drip_core/src -Isrc \
  sim/sim.cpp lib/drip_core/src/drip/*.cpp src/font.cpp src/font_data.cpp src/settings.cpp \
  -o web/sim.wasm
echo "web/sim.wasm: $(wc -c < web/sim.wasm) bytes"
