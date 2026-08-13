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
ENUMERATE_TIMEOUT = 10.0  # how long to wait for USB to come back after reset


def wait_for_port(port_name, timeout):
    """Block until port_name is enumerated again, or timeout elapses.

    Resetting the chip drops the USB-Serial-JTAG device off the bus and Windows
    takes a second or two to re-enumerate it. Without this wait esptool opens the
    port during that gap and dies with "the port doesn't exist".
    """
    from serial.tools import list_ports

    deadline = time.time() + timeout
    while time.time() < deadline:
        if any(p.device == port_name for p in list_ports.comports()):
            return True
        time.sleep(0.1)
    return False


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
    finally:
        port.close()

    # The reset re-enumerates the USB device; give it time to come back before
    # handing the port to esptool.
    time.sleep(0.5)
    if wait_for_port(port_name, ENUMERATE_TIMEOUT):
        time.sleep(0.3)  # settle after the port reappears
    else:
        print("  %s did not come back within %.0fs" % (port_name, ENUMERATE_TIMEOUT))


env.AddPreAction("upload", enter_download_mode)
