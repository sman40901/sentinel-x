# Sentinel-X breadboard reference

Open https://wokwi.com/projects/new/esp32, select the `diagram.json` editor tab,
select all its text, and paste this folder's `diagram.json`. Click inside the
diagram, press F to fit, and use its three-dot menu to enter full screen.
This file is the importable wiring diagram; no simulator account is required to
view it. Saving an online project may require signing in. Uploading a file with
an existing filename did not replace it in the checked web editor; paste instead.

## Start with the separate views

Paste one file at a time into Wokwi's `diagram.json` tab:

1. `01-power.json`: only the supply rails and ESP32.
2. `02-temperature.json`: power plus temperature sensor and its pull-up resistor.
3. `03-motion.json`: power plus motion sensor.
4. `04-gas.json`: power plus gas sensor and its voltage divider.
5. `05-joystick.json`: power plus joystick.
6. `diagram.json`: all connections together.

The sensor views are independent, not cumulative. Components keep the same
positions and breadboard hole numbers so you can assemble one circuit at a time.
The sensor views do not tell you to remove connections already assembled.
Use full screen and zoom as needed; the full drawing has deliberately separated
wire lanes. Light blue = motion, green = temperature, yellow = gas, magenta =
joystick X, cyan = joystick Y, pink = joystick button. Grey wires are GND.

## What the drawing represents

- Classic 30-pin ESP32 DevKit V1; the drawn USB socket differs from USB-C clones.
  Follow GPIO labels, not physical left/right positions.
- DHT22 is a placeholder for the unidentified temperature/humidity sensor.
- MQ2 is a placeholder for the Flying Fish gas module. Read the MQ number on
  the metal sensor can before choosing its supply and warm-up procedure.
- PIR is a visual equivalent for the reported HW-416A. Confirm VCC/OUT/GND and
  its output voltage on the actual module.
- Joystick assumes a passive five-pin VCC/GND/VRx/VRy/SW module, powered at 3.3 V.
- No OLED, buzzer, or LEDs are assumed present.

The placeholder circuits illustrate the intended design, not confirmed pinouts
for the unidentified real hardware. Do not energize unknown sensor pins.

## Resistors and breadboard holes

Use three 5 kOhm resistors (marked 5k), or three 4.7 kOhm if that is the actual
label. For the gas divider, both resistors must have the same value.
`210R` means 210 Ohm; `100k` means 100,000 Ohm. They are not interchangeable.

This drawing uses a full-size, 63-column breadboard. Holes a-e sharing a number
are internally connected. Holes f-j with that same number form a different
connection. The centre trench separates them. Wokwi calls a-e the `t` half and
f-j the `b` half. For example, `bb:40t.c` means physical hole c40.

| Part | First leg | Second leg | Purpose |
| --- | --- | --- | --- |
| R1 / rPullup, 5k | c6 | c12 | DHT data pulled up to 3.3 V |
| R2 / rTop, 5k | c40 | c46 | Gas AO to divider junction |
| R3 / rBottom, 5k | d46 | d52 | Divider junction to GND |

Gas AO wire enters a40. GPIO34 connects at e46. Ground leaves e52. R2 and R3
share column 46 using different holes. Never place both legs of a resistor in
one internally joined five-hole group. The 1:1 divider turns 5 V into nominal
2.5 V; check real voltages with a multimeter.

## Connection order

1. Leave USB and the sensor supply disconnected while assembling.
2. Put the ESP32 beside the breadboard and use female-to-male jumpers if its
   width blocks the holes. Parts drawn above/beside the breadboard use jumpers;
   only the three resistors are shown inserted directly into numbered holes.
3. ESP32 3V3 -> top positive rail (orange). ESP32 GND -> top negative rail.
4. Join top and bottom negative rails at the right edge (grey). All GNDs share
   this connection. Keep the two positive rails separate.
5. The bottom positive rail represents an external regulated 5 V sensor supply;
   connect its negative to the bottom negative rail. Do not connect its positive
   to ESP32 VIN while USB powers the board. Sensor power must be off when the
   ESP32 is unpowered. Bridge interrupted rail segments on a real breadboard
   after checking continuity; some physical breadboards have split rails.
6. Once identified, wire DHT VCC to a6 (fed from top positive), data to a12,
   ground to top negative, and e12 to GPIO27. Install R1. DHT22 pin NC is unused.
7. Once confirmed, wire PIR VCC to bottom positive, GND to top negative, OUT to
   a26, and e26 to GPIO26. A direct OUT connection requires 3.3 V logic.
8. Once confirmed as a suitable 5 V module, wire MQ VCC to bottom positive,
   GND to top negative, and AO through R2/R3 as described above. Leave DO unused.
9. Joystick VCC -> top positive; GND -> top negative; VRx/HORZ -> a58,
   then e58 -> GPIO32; VRy/VERT -> a59, then e59 -> GPIO33; SW/SEL -> a60,
   then e60 -> GPIO25. Software should
   configure the button input as INPUT_PULLUP.
10. Check pin labels, power rail continuity, supply polarity and divider voltage
    before attaching the GPIO34 jumper. Test one sensor at a time.

The camera plugs into the server computer's USB port. Wi-Fi and Bluetooth are
already inside the ESP32 and need no breadboard wires.

## Simulation limits

This is primarily a visual assembly reference. Wokwi documents limited analog
resistor simulation: do not infer the real divider voltage, sensor calibration,
power capacity, or absence of shorts from a successful simulation. The default
new-project sketch prints a greeting; it does not implement these sensors.

## Separate real-board radio test

1. Download Arduino IDE 2 from https://www.arduino.cc/en/software/ for your OS.
2. Install and open it. In Preferences, add the official stable ESP32 board URL
   to “Additional boards manager URLs”:
   `https://espressif.github.io/arduino-esp32/package_esp32_index.json`.
3. Open Boards Manager, search `esp32`, and install the package by
   **Espressif Systems**. This sketch targets its 3.x API. WiFi and BLE libraries
   come with it; do not install unrelated libraries with similar names.
4. Connect the ESP32 with a USB-C **data** cable. No sensors are needed for this
   first test. Select its serial port and the matching classic ESP32 board;
   `ESP32 Dev Module` is a usual choice for a compatible generic DevKit V1.
5. Open `firmware/radio_scan/radio_scan.ino` in this repository and click Verify.
6. Click Upload. If it waits at “Connecting”, hold BOOT until uploading starts,
   then release it. Some boards reset automatically; otherwise press EN after
   upload. If no port appears, check the cable and identify the board's USB
   serial chip before choosing a driver.
7. Open Serial Monitor and set **115200 baud**. The sketch prints Wi-Fi access
   points followed by BLE advertisements, then repeats after a pause.

No Wi-Fi password is needed. This sketch has been source-reviewed but has not
been compiled or run on your hardware in this workspace.

The sketch alternates passive Wi-Fi access-point scans and passive BLE scans,
prints observations locally, and clears each scan's results. It does not
connect to discovered devices, pair, or send results to a service. Names are
untrusted and sanitized before printing. It is a standalone diagnostic, not
integrated into the sensor/MQTT firmware; flashing it replaces the prior sketch.

Wi-Fi results are 2.4 GHz access points/hotspots, not every connected device.
BLE results require an advertising device; Classic Bluetooth inquiry is not
implemented. Names may be absent and addresses may rotate. RSSI is signal
strength, not reliable distance; neither radio reliably counts people.
Wokwi in a browser cannot scan the real room's radios. Use the physical ESP32.

## References

- https://docs.wokwi.com/diagram-format
- https://docs.wokwi.com/guides/diagram-editor
- https://docs.wokwi.com/parts/wokwi-resistor
- https://docs.wokwi.com/parts/wokwi-analog-joystick
- https://docs.espressif.com/projects/arduino-esp32/en/latest/installing.html
- https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-guides/coexist.html
