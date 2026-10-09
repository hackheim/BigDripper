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
1. Each message prints in the font picked above the text box: Spleen (the original Spleen 8x16 font), Drip or Drip bold. Drip and Drip bold have A-Z, ÆØÅ, 0-9 and `. , ! ? - : '`; Spleen only has A-Z, 0-9 and space. Anything a font doesn't have prints as a space. Emoji are written as shortcodes (the web page has a button for each): `:smile:` `:sad:` `:big_smile:` `:neutral:` `:confused:` `:wink:` `:surprised:` `:tongue:` `:heart:` `:drop:` `:star:`. The length limit counts printed characters, so an emoji counts as one.

## Font

The Drip glyphs are defined in `font_design/gen_font.py`, and the Spleen font's bitmaps in `font_design/spleen_8x16.py`. After changing it, regenerate the preview and the firmware tables from this directory:

    python3 font_design/gen_font.py font_design/font_preview.txt
    python3 font_design/gen_font.py --c src/font_data.cpp

`font_design/host_test/` builds `font.cpp` on the host (needs `g++`) and checks the fonts, the emoji shortcodes and the length counting:

    sh font_design/host_test/run.sh

## Debugging

1. Make sure that the CP210x driver is installed oin the machine you are debugging on. If not, the UART port will not enumerate










