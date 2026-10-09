# Firmware demo

This code contains logic for controlling 16 valves and also reading input from a quadrature encoder. Valve control and quadrature logic is running on separate cores

## Building and flashing

1. Disconnect USB port
1. Press BOOT button
1. Reconnect USB port
1. pio run -t upload -t monitor
1. Press "Reset" on the ESP32

## Text input

1. Connect to the BigDripper SSID and enter the password "dripdrip1"
1. Go to http://bigdripper.local (fallback http://192.168.4.1) to queue text to print
1. Go to http://bigdripper.local/params for scaling/length/pause settings (user "admin", password "letmeprint9")
1. Each message prints in the font picked above the text box: Spleen (the original Spleen 8x16 font), Drip or Drip bold. Drip and Drip bold have A-Z, ÆØÅ, 0-9 and `. , ! ? - : '`; Spleen only has A-Z, 0-9 and space. Anything a font doesn't have prints as a space. The Invert button next to the fonts prints the message inverted: dry letters in a sprayed band, with 2 solid columns on each side. This uses a lot more water than normal text. Emoji are written as shortcodes (the web page has a button for each): `:smile:` `:sad:` `:big_smile:` `:neutral:` `:confused:` `:wink:` `:surprised:` `:tongue:` `:heart:` `:drop:` `:star:`. The length limit counts printed characters, so an emoji counts as one.

## Font

The Drip glyphs are defined in `font_design/gen_font.py`, and the Spleen font's bitmaps in `font_design/spleen_8x16.py`. After changing it, regenerate the preview and the firmware tables from this directory:

    python3 font_design/gen_font.py font_design/font_preview.txt
    python3 font_design/gen_font.py --c src/font_data.cpp

`font_design/host_test/` builds `font.cpp` on the host (needs `g++`) and checks the fonts, the emoji shortcodes and the length counting:

    sh font_design/host_test/run.sh

## Console preview

Check what the head prints at the bench, without water. Two extra build environments draw every column that fires on the serial console as one line of text; turn the wheel and the message scrolls by column by column.

    pio run -e preview -t upload -t monitor        # preview, valves fire as normal
    pio run -e preview-dry -t upload -t monitor    # preview, valves never open (dry run)

Add `--upload-port` / `--monitor-port` if the CP2102 isn't at the port in `platformio.ini`. On Linux it's usually `/dev/ttyUSB*` (e.g. `--upload-port /dev/ttyUSB1`), or watch it with `pio device monitor -p /dev/ttyUSB1 -b 115200`. If `pio` isn't on your PATH, it's `~/.platformio/penv/bin/pio`. A plain `pio run` still builds only the normal firmware. **Flash the normal firmware (`pio run -t upload`) again before riding**: a dry-run board prints `*** DRY RUN: valves disabled ***` at boot and its web page shows a red "DRY RUN — valves disabled" line.

The preview is on the CP2102 port ("UART", the one used for flashing; `Serial0` in the code), not the native USB port. If nothing shows up, check which port you're watching first.

How to read it: each line is one column, 16 characters, `#` = valve open, `.` = closed, COIL1 on the left. Columns appear top to bottom in the order they fire. **Rotate the screen 90° anticlockwise** (left edge down) and the text reads the right way up, first column on the left. It shows the coil bits actually written, so if the preview reads mirrored, the ground print is mirrored too. `HI` in Drip looks like this:

    -- HI [Drip] --
    ################
    .......##.......
    .......##.......
    .......##.......
    .......##.......
    .......##.......
    .......##.......
    .......##.......
    ################
    ................
    ................
    ................
    ##............##
    ##............##
    ################
    ##............##
    ##............##

Other lines: `-- mode: lines --` when the mode changes (Prime and Trace draw nothing, Lines draws its columns), `~~ skipped 3 ~~` when the wheel jumped past columns between two scans (those never fire on the ground either), and `!! dropped 12 lines` if the console fell behind a fast spin. Dropped lines only affect the preview, never the valves.

Under the hood these are the build flags `-D CONSOLE_PREVIEW=1` and `-D CONSOLE_PREVIEW_DRY_RUN=1`, which also work on their own.

## Debugging

1. Make sure that the CP210x driver is installed oin the machine you are debugging on. If not, the UART port will not enumerate










