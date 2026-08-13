"""Force the ESP32-S3 into download mode before esptool connects.

esptool's own USBJTAGSerialReset sequence (esptool/reset.py) uses 0.1 s settle
delays between DTR/RTS transitions. On this host's USB-Serial-JTAG driver that is
too tight: the chip resets but GPIO0 is not held low, so it comes up in SPI boot
(esptool reports "Wrong boot mode detected (0x8)") and the upload fails.

The same sequence with longer delays reaches download mode reliably. This runs it
pre-upload, and platformio.ini sets board_upload.before_reset = no_reset so
esptool connects to the bootloader we just entered instead of resetting again.

Delete this script and the board_upload.before_reset line together if a future
esptool makes them unnecessary.
"""

import time

Import("env")

SETTLE = 0.3  # esptool uses 0.1; that is what fails here


def enter_download_mode(source, target, env):
    import serial

    env.AutodetectUploadPort()
    port_name = env.subst("$UPLOAD_PORT")
    print("Forcing download mode on %s (settle %.2fs)" % (port_name, SETTLE))

    try:
        port = serial.Serial(port_name, 115200, timeout=1)
    except Exception as exc:
        print("  could not open %s: %s" % (port_name, exc))
        print("  is a serial monitor still attached?")
        return

    try:
        # esptool's USBJTAGSerialReset, with SETTLE in place of its 0.1 s sleeps.
        port.setRTS(False)
        port.setDTR(False)  # idle
        time.sleep(SETTLE)
        port.setDTR(True)  # IO0 low
        port.setRTS(False)
        time.sleep(SETTLE)
        port.setRTS(True)  # into reset, via (1,1) rather than (0,0)
        port.setDTR(False)
        port.setRTS(True)  # Windows only propagates DTR when RTS is set
        time.sleep(SETTLE)
        port.setDTR(False)
        port.setRTS(False)  # out of reset, into the ROM bootloader
        time.sleep(0.4)
    finally:
        port.close()


env.AddPreAction("upload", enter_download_mode)
