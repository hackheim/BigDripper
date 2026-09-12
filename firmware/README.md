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
1. Go to http://bigdripper.local (fallback http://192.168.4.1)







