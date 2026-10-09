#!/bin/sh
# Builds font.cpp + font_data.cpp on the host against a stub Arduino.h and
# runs font_test.cpp. Run from firmware/: sh font_design/host_test/run.sh
set -e
here=$(dirname "$0")
out=${TMPDIR:-/tmp}/bigdripper_font_test
g++ -std=c++17 -Wall -I"$here" -Isrc "$here/font_test.cpp" src/font.cpp src/font_data.cpp -o "$out"
"$out"
