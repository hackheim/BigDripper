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
1. Text can use A-Z, ÆØÅ, 0-9 and `. , ! ? - : '`. Put `*stars*` around words to print them bold. Emoji: `:-)` `:-(` `:-D` `:-|` `:-/` `;-)` `:-O` `:-P` `<3` `:drop:` `:star:`

## Font

The glyphs are defined in `font_design/gen_font.py`. After changing it, regenerate the preview and the firmware tables from this directory:

    python3 font_design/gen_font.py font_design/font_preview.txt
    python3 font_design/gen_font.py --c src/font_data.cpp

## Debugging

1. Make sure that the CP210x driver is installed oin the machine you are debugging on. If not, the UART port will not enumerate










