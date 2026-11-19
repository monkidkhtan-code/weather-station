# ESP32 Weather & Climate Station

Complete hardware, firmware, and separate web app + SQLite backend designed for your 30-Pin ESP32 + 30P Expansion Board, ready to link with a future soil moisture node.

## 1. Wiring on Your Black 30P Expansion Board

Keep the **`JUMP`** jumper on **`3.3V`** (bottom two pins).
On the expansion board's 3-pin columns, **`G`** = Ground, **`V`** = 3.3V (via `JUMP`), **`S`** = GPIO Signal.

| Component | Module Pin | Expansion Board Connection |
| :--- | :--- | :--- |
| **DHT22** | `VCC` / `+` | `D4` column → `V` (3.3V) |
| | `GND` / `-` | `D4` column → `G` (GND) |
| | `DATA` / `OUT` | `D4` column → `S` (GPIO 4) |
| **BH1750** | `VCC` | Top-left `3.3V` header |
| | `GND` | Top-left `GND` header |
| | `SDA` | `D21` column → `S` (GPIO 21) |
| | `SCL` | `D22` column → `S` (GPIO 22) |
| | `ADDR` | Any `GND` (`G`) pin |
| **4-Pin I2C LCD** | `GND` | Bottom-left I2C header → `GND` |
| | `VCC` | Bottom-left I2C header → `VCC` *(or `VIN`/5V if contrast is dim)* |
| | `SDA` | Bottom-left I2C header → `D21` |
| | `SCL` | Bottom-left I2C header → `D22` |
| **Raindrops Module** | `VCC` | `D34` column → `V` (3.3V) |
| | `GND` | `D34` column → `G` (GND) |
| | `AO` | `D34` column → `S` (GPIO 34, ADC1) |
| **Voltage Sensor** | `VCC (+)` / `GND (-)` | 18650 battery `+` and `-` |
| | `S` (Signal) | `D35` column → `S` (GPIO 35, ADC1) |
| | `-` | `D35` column → `G` (GND) |

## 2. Run the Separate Backend & Web App

From the project folder:

```powershell
python server/app.py
```

Open `http://localhost:8000` in your browser.
To find your PC's LAN IP for the ESP32, run `ipconfig` and look for your Wi-Fi `IPv4 Address` (e.g. `192.168.1.100`).

## 3. Flash the ESP32 Firmware

1. Open `firmware/weather_station/weather_station.ino` in **Arduino IDE**.
2. Install these libraries via **Library Manager**:
   * `DHT sensor library` (by Adafruit) + `Adafruit Unified Sensor`
   * `BH1750` (by Christopher Laws)
   * `LiquidCrystal_I2C` (by Frank de Brabander)
   * `ArduinoJson` (by Benoit Blanchon)
3. Update `WIFI_SSID`, `WIFI_PASSWORD`, and `SERVER_URL` at the top of `weather_station.ino`.
4. Select board **ESP32 Dev Module**, plug USB into the **ESP32's USB-C port**, and click **Upload**.
5. Once tested, unplug your laptop USB and plug in your **18650 Battery Shield V3** USB cable.

## 4. Linking Your Future Moisture Sensor

You never need to re-flash `weather-01` to update the dashboard or add moisture logic. Simply have your future moisture node `POST` JSON to `http://<PC_IP>:8000/api/telemetry`:

```json
{
  "device_id": "moisture-01",
  "device_type": "moisture",
  "soil_moisture_pct": 42.5
}
```
