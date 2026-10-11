#!/bin/sh
# Builds the print engine core on the host and runs its tests. Run from
# firmware/: sh lib/drip_core/test/run.sh
#
# gnu++11 because that's what the ESP32 Arduino core compiles with, so
# anything that passes here also builds for the board.
set -e
here=$(dirname "$0")
out=${TMPDIR:-/tmp}/bigdripper_core_test
g++ -std=gnu++11 -Wall -Wextra -Werror -I"$here/../src" "$here"/*_test.cpp "$here/test_main.cpp" \
    "$here"/../src/drip/*.cpp -o "$out"
"$out"
