/*
 * ESP32 Weather & Climate Station Firmware
 * Hardware:
 *   - ESP32 30-Pin DevKit on 30P Expansion Board (JUMP set to 3.3V)
 *   - DHT22 Temperature & Humidity Sensor -> GPIO 4
 *   - BH1750 Light Intensity Sensor (I2C 0x23) -> GPIO 21 (SDA), GPIO 22 (SCL)
 *   - 16x2 I2C Character LCD (0x27 or 0x3F) -> GPIO 21 (SDA), GPIO 22 (SCL)
 *   - Raindrops Detection Sensor (AO) -> GPIO 34 (ADC1)
 *   - Voltage Sensor Module (5:1 divider) -> GPIO 35 (ADC1)
 *
 * Required Arduino Libraries:
 *   - DHT sensor library (Adafruit) + Adafruit Unified Sensor
 *   - BH1750 (Christopher Laws)
 *   - LiquidCrystal_I2C (Frank de Brabander)
 *   - ArduinoJson (Benoit Blanchon)
 */

#include <WiFi.h>
#include <WiFiMulti.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <DHT.h>
#include <BH1750.h>
#include <LiquidCrystal_I2C.h>
#include <ArduinoJson.h>

// ===================== USER CONFIGURATION =====================
// Network 1: Farm Wi-Fi & Server
const char* FARM_SSID       = "KHAGRIFARM_2.4";
const char* FARM_PASSWORD   = "123123farm";
const char* FARM_SERVER_URL = "http://192.168.0.219:8000/api/telemetry";

// Network 2: Home Wi-Fi & Server
const char* HOME_SSID       = "YOUR_HOME_2.4GHZ_WIFI";
const char* HOME_PASSWORD   = "YOUR_HOME_WIFI_PASSWORD";
const char* HOME_SERVER_URL = "http://192.168.1.100:8000/api/telemetry";

const char* DEVICE_ID       = "weather-01";

// Timing
const unsigned long LCD_INTERVAL_MS  = 2000;   // Update LCD every 2s
const unsigned long SEND_INTERVAL_MS = 10000;  // Send data to server every 10s

// Enable each module as you connect it (prevents noisy readings from empty pins)
#define ENABLE_DHT22   true
#define ENABLE_BH1750  false
#define ENABLE_RAIN    false
#define ENABLE_BATTERY false
// ==============================================================

// Pin Definitions
#define DHT_PIN       4
#define DHT_TYPE      DHT22
#define RAIN_AO_PIN   34   // ADC1 (Wi-Fi safe)
#define VOLT_PIN      35   // ADC1 (Wi-Fi safe)

// Sensor Calibration
#define LCD_I2C_ADDR  0x27 // Change to 0x3F if your LCD screen stays blank
#define RAIN_DRY_ADC  4095 // Typical dry reading (~3.3V)
#define RAIN_WET_THRESHOLD 2600 // Below this ADC value counts as raining
#define VOLTAGE_DIVIDER_RATIO 5.0 // Standard 0-25V module (30k + 7.5k)
#define ADC_CALIBRATION_FACTOR 1.0 // Adjust slightly after comparing with multimeter

DHT dht(DHT_PIN, DHT_TYPE);
BH1750 lightMeter(0x23);
LiquidCrystal_I2C lcd(LCD_I2C_ADDR, 16, 2);

struct SensorReading {
  float tempC;
  float humidity;
  float lux;
  int rainRaw;
  int rainPercent;
  bool isRaining;
  float batteryV;
  int batteryPercent;
  bool dhtValid;
};

unsigned long lastLcdUpdate = 0;
unsigned long lastSendTime  = 0;
bool showSecondPage = false;

WiFiMulti wifiMulti;
bool wifiInitialized = false;

const char* getServerUrl() {
  if (WiFi.SSID() == HOME_SSID) {
    return HOME_SERVER_URL;
  }
  return FARM_SERVER_URL;
}

void connectWiFi() {
  if (!wifiInitialized) {
    WiFi.mode(WIFI_STA);
    wifiMulti.addAP(FARM_SSID, FARM_PASSWORD);
    wifiMulti.addAP(HOME_SSID, HOME_PASSWORD);
    wifiInitialized = true;
  }

  if (wifiMulti.run() == WL_CONNECTED) {
    Serial.println("Wi-Fi connected (" + WiFi.SSID() + ")! IP: " + WiFi.localIP().toString());
  } else {
    Serial.println("Wi-Fi not connected yet; will retry on next send.");
  }
}

float readAveragedPinVoltage(int pin, int samples = 10) {
  long totalMv = 0;
  for (int i = 0; i < samples; i++) {
    totalMv += analogReadMilliVolts(pin);
    delay(5);
  }
  return (totalMv / (float)samples) / 1000.0;
}

int readAveragedRaw(int pin, int samples = 10) {
  long total = 0;
  for (int i = 0; i < samples; i++) {
    total += analogRead(pin);
    delay(5);
  }
  return total / samples;
}

int estimateBatteryPercent(float v) {
  if (v <= 3.0) return 0;
  if (v >= 4.2) return 100;
  return (int)(((v - 3.0) / (4.2 - 3.0)) * 100.0);
}

SensorReading readSensors() {
  SensorReading r{};

  if (ENABLE_DHT22) {
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    r.dhtValid = !(isnan(t) || isnan(h));
    r.tempC    = r.dhtValid ? t : 0.0;
    r.humidity = r.dhtValid ? h : 0.0;
  }

  if (ENABLE_BH1750) {
    r.lux = lightMeter.readLightLevel();
    if (r.lux < 0) r.lux = 0.0;
  }

  if (ENABLE_RAIN) {
    r.rainRaw = readAveragedRaw(RAIN_AO_PIN);
    r.rainPercent = constrain(map(r.rainRaw, RAIN_DRY_ADC, 800, 0, 100), 0, 100);
    r.isRaining = (r.rainRaw < RAIN_WET_THRESHOLD);
  }

  if (ENABLE_BATTERY) {
    float pinV = readAveragedPinVoltage(VOLT_PIN);
    r.batteryV = pinV * VOLTAGE_DIVIDER_RATIO * ADC_CALIBRATION_FACTOR;
    r.batteryPercent = estimateBatteryPercent(r.batteryV);
  }

  return r;
}

void updateLcd(const SensorReading& r) {
  lcd.clear();
  bool hasPage2 = ENABLE_RAIN || ENABLE_BATTERY;

  if (!showSecondPage || !hasPage2) {
    // Page 1: Temp & Humidity / Light (or Wi-Fi IP if BH1750 not connected)
    lcd.setCursor(0, 0);
    if (!ENABLE_DHT22) {
      lcd.print("DHT22 Disabled");
    } else if (r.dhtValid) {
      lcd.printf("T:%.1fC  H:%.0f%%", r.tempC, r.humidity);
    } else {
      lcd.print("DHT22 Read Error");
    }
    lcd.setCursor(0, 1);
    if (ENABLE_BH1750) {
      lcd.printf("Lux:%.0f", r.lux);
    } else if (WiFi.status() == WL_CONNECTED) {
      lcd.print(WiFi.localIP().toString());
    } else {
      lcd.print("WiFi: Offline");
    }
  } else {
    // Page 2: Rain & Battery
    lcd.setCursor(0, 0);
    if (ENABLE_RAIN) {
      lcd.printf("Rain:%s (%d%%)", r.isRaining ? "WET" : "DRY", r.rainPercent);
    } else {
      lcd.print("Rain: --");
    }
    lcd.setCursor(0, 1);
    if (ENABLE_BATTERY) {
      lcd.printf("Bat:%.2fV %d%%", r.batteryV, r.batteryPercent);
    } else {
      lcd.print("Bat: USB Power");
    }
  }
  showSecondPage = !showSecondPage;
}

void sendTelemetry(const SensorReading& r) {
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
    if (WiFi.status() != WL_CONNECTED) return;
  }

  JsonDocument doc;
  doc["device_id"]   = DEVICE_ID;
  doc["device_type"] = "weather";
  doc["dht_valid"]   = r.dhtValid;
  if (ENABLE_DHT22) {
    doc["temp_c"]    = round(r.tempC * 10.0) / 10.0;
    doc["humidity"]  = round(r.humidity * 10.0) / 10.0;
  }
  if (ENABLE_BH1750) {
    doc["lux"]       = round(r.lux * 10.0) / 10.0;
  }
  if (ENABLE_RAIN) {
    doc["rain_raw"]   = r.rainRaw;
    doc["rain_pct"]   = r.rainPercent;
    doc["is_raining"] = r.isRaining;
  }
  if (ENABLE_BATTERY) {
    doc["battery_v"]   = round(r.batteryV * 100.0) / 100.0;
    doc["battery_pct"] = r.batteryPercent;
  }

  String payload;
  serializeJson(doc, payload);

  const char* targetUrl = getServerUrl();
  HTTPClient http;
  http.begin(targetUrl);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(4000);

  int httpCode = http.POST(payload);
  if (httpCode > 0) {
    Serial.printf("POST %s -> HTTP %d\n", targetUrl, httpCode);
  } else {
    Serial.printf("POST failed: %s\n", http.errorToString(httpCode).c_str());
  }
  http.end();
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== ESP32 Weather Station Starting ===");

  analogReadResolution(12);
  Wire.begin(21, 22);

  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("Weather Station");
  lcd.setCursor(0, 1);
  lcd.print("Connecting WiFi");

  if (ENABLE_DHT22) {
    dht.begin();
  }
  if (ENABLE_BH1750) {
    if (!lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE)) {
      Serial.println("Warning: BH1750 not found! Check SDA=21, SCL=22, ADDR=GND");
    }
  }

  connectWiFi();
}

void loop() {
  unsigned long now = millis();

  if (now - lastLcdUpdate >= LCD_INTERVAL_MS) {
    lastLcdUpdate = now;
    SensorReading r = readSensors();
    updateLcd(r);

    if (now - lastSendTime >= SEND_INTERVAL_MS) {
      lastSendTime = now;
      sendTelemetry(r);
    }
  }
}
