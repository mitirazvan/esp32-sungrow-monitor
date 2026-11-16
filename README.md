# Sungrow PV Monitor (ESP32-C3 + 16x2 LCD)

A small ESP32-C3 device that reads live data from a **Sungrow SH-series hybrid inverter**
over **Modbus TCP** and shows it on a 16x2 I2C LCD and on a web page.

```
☀5.23kW  🔋85% ↑✓        ☀ solar   🔋 battery (↑ charging, ↓ discharging)
⌂1.20kW  ⏚0.44kW ↓        ⌂ house load   ⏚ grid (↓ import, ↑ export)
```
(The icons are custom LCD characters; the top-right icon is the link state:
check = data loaded, circular arrow = syncing, X = offline.)

## Features
- PV power, house load, grid import/export, battery level and charge/discharge direction
- Values under 1000 shown in W, above in kW
- Web page (`http://<device-ip>/` or `http://pv.local`) with a phone-friendly dashboard
- JSON API at `/api`
- WiFi setup portal (access point `PV-Setup`) with an inverter IP field; reopens by itself if WiFi is lost for 60 s
- Inverter discovery: scans your network for the inverter, or set the IP at `/settings`
- Self-recovery watchdog and detailed Serial log for troubleshooting
- No Modbus library needed (small hand-written Modbus TCP client, read-only)

## Hardware / devices involved

| Device | Role |
|--------|------|
| **Sungrow SH6.0RS** hybrid inverter | Source of the data (PV, load, grid, battery) |
| **Sungrow WiNet-S** WiFi/Ethernet dongle | Exposes the inverter's Modbus TCP server on port 502 |
| **ESP32-C3 Super Mini** | Polls the inverter, drives the LCD, serves the web page |
| **1602 (16x2) LCD with I2C backpack** (PCF8574, address 0x27/0x3F) | Local display |
| Optional push button (GPIO3 to GND) | Erases saved WiFi at power-up |
| USB-C power supply (5 V) | Powers the ESP32 and LCD |
| WiFi router | Connects the ESP32 and the inverter dongle on the same network |
| Phone / PC (optional) | Web dashboard and setup portal |

**3D-printed case:** [LCD enclosure 1602 (including BME280 sensor mount)](https://makerworld.com/en/models/1486976-lcd-enclosure-1602-including-bme280-sensor-mount)
on MakerWorld. The model includes a mount for a BME280 sensor, which this firmware does not use.
Check the model page for its license and print settings.

Wiring and build notes: [WIRING.md](WIRING.md)

## Compatibility
Developed and tested on a **Sungrow SH6.0RS** connected through its WiNet-S dongle
(Modbus TCP, port 502, unit id 1). Other SH-series hybrids probably use the same
registers, but this is **not verified**. The register addresses are `#define`s at the top of
the sketch (`REG_*`, with `REG_OFFSET` to shift them by one if your values look off).

The sketch only **reads** registers (Modbus function 04). It never writes to the inverter.

## Build and flash
1. Install the **esp32 by Espressif Systems** board package.
2. Install the libraries **WiFiManager** (tzapu, 2.0.17+) and **LiquidCrystal I2C** (Frank de Brabander).
3. Board: **ESP32C3 Dev Module**, **USB CDC On Boot: Enabled**.
4. Open `ModBusPV/ModBusPV.ino`, set `DEFAULT_INVERTER_IP` (or leave it and use the setup portal), upload.

Works with the Arduino IDE or VS Code with the Arduino extension.

## First start
1. Join the WiFi `PV-Setup` (password in the sketch, `AP_PASS`) and open `http://192.168.4.1`.
2. Choose your WiFi, enter its password, optionally enter the inverter IP, save.
3. The LCD shows the data; the device address is printed on the Serial monitor (115200).
   Change the AP password in the sketch before flashing if the device is used outside a trusted place.

Hold a button on GPIO3 to GND while powering up to erase the saved WiFi.

## Notes
- The WiNet-S allows only a few simultaneous Modbus connections. The device opens one connection per poll (every 5 s).
- Serial log (115200) prints every poll, WiFi events and a 30 s heartbeat, which helps diagnose hangs.
- Use at your own risk. This project is not affiliated with Sungrow.

## License
See [LICENSE](LICENSE).
