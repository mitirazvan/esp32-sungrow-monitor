# Wiring: ESP32-C3 Super Mini + 16x2 I2C LCD (breadboard)

## Parts
- ESP32-C3 Super Mini
- 16x2 LCD with I2C backpack (PCF8574, 4 pins: GND, VCC, SDA, SCL)
- Breadboard + 4 jumper wires (male-to-male, or male-to-female if the LCD pins are headers)
- Optional: push button + 1 wire (WiFi reset)

## Connections

| LCD backpack | ESP32-C3 Super Mini |
|--------------|---------------------|
| GND          | GND                 |
| VCC          | 5V                  |
| SDA          | GPIO8               |
| SCL          | GPIO9               |

Optional reset button: one leg to GPIO3, the other leg to GND.
Hold it while plugging in USB to erase saved WiFi and reopen the config portal.

## Step by step
1. Plug the ESP32-C3 into the breadboard straddling the center gap, so the pins on
   the two sides are on different rows. Leave the USB port at the board's edge.
2. Use the breadboard's power rails: wire ESP32 `GND` to the blue (-) rail and
   `5V` to the red (+) rail.
3. LCD `GND` -> blue rail. LCD `VCC` -> red rail (5V).
4. LCD `SDA` -> ESP32 `GPIO8` (labelled `8`).
5. LCD `SCL` -> ESP32 `GPIO9` (labelled `9`).
6. Plug in USB-C. The backlight should come on. Turn the blue potentiometer on the
   backpack with a small screwdriver until the characters show (no text = contrast too low).

## Notes
- **Power from 5V, not 3.3V.** Most 1602 LCDs are unreliable at 3.3V. The
  `5V` pin on the Super Mini is USB VBUS, so it is only live when USB is plugged in.
- **Logic levels:** the ESP32 is 3.3V. The backpack's SDA/SCL pull-ups usually go to
  VCC (5V). This normally works in hobby use, but for a permanent build,
  remove the two pull-up resistors on the backpack (or use a level shifter) and
  rely on 3.3V pull-ups.
- **I2C address** is 0x27 or 0x3F. The sketch scans and picks it automatically.
  The address is also printed on the Serial monitor.
- GPIO8 also drives the onboard LED, so the LED may flicker with I2C traffic. This is normal.
- If you use other pins, change `SDA_PIN`/`SCL_PIN` in the sketch.

## Arduino IDE setup
1. Boards Manager: install **esp32 by Espressif Systems**.
2. Board: **ESP32C3 Dev Module**. Set **USB CDC On Boot: Enabled**.
3. Library Manager: install **WiFiManager** (tzapu) and **LiquidCrystal I2C** (Frank de Brabander).
4. If upload fails, hold **BOOT**, tap **RESET**, release BOOT, then upload again.
5. Open `ModBusPV.ino` and upload.

## Using it
1. After the first boot the LCD shows `AP: PV-Setup` and `192.168.4.1`.
2. On your phone or PC, join WiFi `PV-Setup` (password `12345678`).
3. A captive portal opens (or browse to http://192.168.4.1). Click **Configure WiFi**,
   pick your network, enter the password, and save.
4. The device reboots onto your network, shows its IP briefly, then the solar data screen.
   See [README.md](README.md) for the display layout, web page and inverter setup.
