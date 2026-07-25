# Big Dripper 

### BigDripper – Controller Board and Operating Principle

BigDripper is Hackheim’s version of the classic bicycle-mounted water printer (see links below). The printer is being developed as a community project, with the goal of having it completed by the end of September 2026.

The printer consists of 16 solenoid valves mounted in a row across the bicycle trailer. The valves are supplied with water from a tank and pump. When a valve is opened briefly, it releases a small amount of water onto the ground, forming a single pixel. Together, the 16 valves produce one vertical column of 16 pixels. As the bicycle moves forward and new columns are printed in sequence, the trailer effectively operates as a large-scale dot-matrix printer.

An encoder measures the distance travelled by the bicycle. Rather than advancing through the image at fixed time intervals, the controller selects the next column after the bicycle has travelled a predefined distance. This helps maintain the proportions of the printed text or graphics even when the cycling speed varies.

### Controller board

The controller is based on an ESP32-S3 DevKitC-1, which plugs into a custom-designed carrier board. To make the board easy to maintain and repair—and also suitable for educational use and soldering workshops—we have chosen to use through-hole components throughout the carrier board.

The board has a nominal input voltage of 12 V, which is also the operating voltage of the solenoid valves. A TL2575-based switching regulator generates 5 V for the ESP32 development board and the other logic circuitry.

The regulator itself supports an input voltage of up to 40 V. However, the input voltage is also supplied directly to the solenoid valves. When using a voltage higher than 12 V, both the valves and all other components connected to the input-voltage rail must therefore be rated for the selected voltage.

[controller](./hardware/pictures/controller.jpg)

### Solenoid valve control

The 16 valve outputs are controlled by IRL540N MOSFETs in a low-side switching configuration. The control signals from the ESP32 use 3.3 V logic and are therefore routed through SN74HCT125NE4 buffers powered from 5 V. These buffers provide logic-level translation and drive the MOSFET gates at a higher and more suitable voltage.

Each MOSFET gate is connected to its buffer output through a 33-ohm series resistor. The control signals also have 47-kilohm pull-down resistors, ensuring that the valves remain switched off while the ESP32 is starting, being reset, or not actively driving the outputs.

Each valve output is protected by a dedicated 1N4007 flyback diode. The diode suppresses the inductive voltage spike generated when the current through the solenoid coil is interrupted, protecting the MOSFET from excessive drain voltage.

### Connections and expansion options

The board has a dedicated input for an encoder with transistor outputs and A/B signals. An additional header provides access to 12 spare GPIO signals, together with 3.3 V, 5 V, and ground connections.

Four separate outputs are available for NeoPixel LEDs. These signals are also level-shifted from 3.3 V to 5 V using an SN74HCT125 buffer, and each data output includes a 330-ohm series resistor.

The board also includes indicator LEDs for the 12 V, 5 V, and 3.3 V supply rails, making it easy to verify that the different supply voltages are present.

All external inputs and outputs use WAGO board-to-wire lever connectors, allowing wires to be connected and removed easily without tools.

### Schematic

[schematic](./hardware/pdf/schematic.pdf)