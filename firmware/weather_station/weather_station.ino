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
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <time.h>
#include <Preferences.h>
#include <DHT.h>
#include <BH1750.h>
#include <LiquidCrystal_I2C.h>
#include <ArduinoJson.h>

#define FW_VERSION "1.1"
#define BOOT_BTN_PIN 0

// ===================== USER CONFIGURATION =====================
// Network 1: Farm Wi-Fi
const char* FARM_SSID       = "KHAGRIFARM_2.4";
const char* FARM_PASSWORD   = "123123farm";

// Network 2: Home Wi-Fi
const char* HOME_SSID       = "YOUR_HOME_2.4GHZ_WIFI";
const char* HOME_PASSWORD   = "YOUR_HOME_WIFI_PASSWORD";

// Firebase Realtime Database (KH Agrifarm - works from both Farm & Home)
const char* FIREBASE_URL        = "https://kh-agrifarm-default-rtdb.asia-southeast1.firebasedatabase.app/weather_devices/weather-01.json";
const char* FIREBASE_CONFIG_URL = "https://kh-agrifarm-default-rtdb.asia-southeast1.firebasedatabase.app/weather_config/weather-01.json";

// Google Sheets Apps Script Web App URL (leave "" to disable until deployed)
const char* GOOGLE_SCRIPT_URL   = "https://script.google.com/macros/s/AKfycbzjwD6LuNSgaC4mDKymHxUg6Ieb14AVkFbJnIxixVzbIAhshHhNFBUpVMl_5tyZ5Sgo/exec";

const char* DEVICE_ID           = "weather-01";

// Default Timing & Modes (controlled remotely via Firebase /weather_config/weather-01)
const unsigned long LCD_INTERVAL_MS   = 2000;    // Update LCD every 2s
unsigned long sendIntervalMs          = 10000;   // read_interval_sec (default 10s, min 5s)
unsigned long sheetIntervalMs         = 300000;  // sheet_interval_sec (default 300s / 5 min)
bool nightEcoEnabled                  = false;   // night_eco_enabled (7 PM - 6 AM)
unsigned long nightEcoIntervalMs      = 1800000; // night_eco_interval_sec (default 30 min)
String lcdMode                        = "on";    // "on" | "text_only" | "flash_sync" | "off"
String powerMode                      = "awake"; // "awake" | "smart_saver" | "deep_sleep"
unsigned long lcdWakeUntilMs          = 0;       // Temp 30s wake when BOOT button pressed

// Enable each module as you connect it (prevents noisy readings from empty pins)
#define ENABLE_DHT22   true
#define ENABLE_BH1750  true
#define ENABLE_RAIN    true
#define ENABLE_BATTERY true
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
unsigned long lastSheetTime = 0;
bool sheetSentOnce = false;
bool sentOnBoot = false;
bool showSecondPage = false;

Preferences prefs;
WiFiMulti wifiMulti;
bool wifiInitialized = false;

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

void applyLcdMode(bool duringSync = false) {
  if (millis() < lcdWakeUntilMs) {
    lcd.display();
    lcd.backlight();
    return;
  }
  // Force LCD completely off in Ultra Deep Sleep unless woken by BOOT button
  if (powerMode == "deep_sleep") {
    lcd.noBacklight();
    lcd.noDisplay();
    return;
  }
  if (lcdMode == "on") {
    lcd.display();
    lcd.backlight();
  } else if (lcdMode == "text_only") {
    lcd.display();
    lcd.noBacklight();
  } else if (lcdMode == "flash_sync") {
    lcd.display();
    if (duringSync) lcd.backlight();
    else lcd.noBacklight();
  } else {
    // "off"
    lcd.noBacklight();
    lcd.noDisplay();
  }
}

void applyPowerMode() {
  if (powerMode == "smart_saver") {
    setCpuFrequencyMhz(80);
    WiFi.setSleep(true);
  } else {
    setCpuFrequencyMhz(240);
    WiFi.setSleep(false);
  }
}

unsigned long getEffectiveIntervalMs() {
  if (nightEcoEnabled) {
    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 10)) {
      int hr = timeinfo.tm_hour;
      if (hr >= 19 || hr < 6) {
        return max(sendIntervalMs, nightEcoIntervalMs);
      }
    }
  }
  return sendIntervalMs;
}

void sendTelemetry(const SensorReading& r) {
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
    if (WiFi.status() != WL_CONNECTED) return;
  }

  if (lcdMode == "flash_sync") applyLcdMode(true);

  JsonDocument doc;
  doc["device_id"]   = DEVICE_ID;
  doc["device_type"] = "weather";
  doc["fw_version"]  = FW_VERSION;
  doc["wifi_rssi"]   = WiFi.RSSI();
  doc["ssid"]        = WiFi.SSID();
  doc["lan_ip"]      = WiFi.localIP().toString();
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
  doc["updated_at"][".sv"] = "timestamp";

  String payload;
  serializeJson(doc, payload);

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.begin(client, FIREBASE_URL);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(5000);

  int httpCode = http.PUT(payload);
  if (httpCode > 0) {
    Serial.printf("PUT Firebase -> HTTP %d\n", httpCode);
  } else {
    Serial.printf("PUT Firebase failed: %s\n", http.errorToString(httpCode).c_str());
  }
  http.end();

  unsigned long now = millis();
  if (GOOGLE_SCRIPT_URL[0] != '\0' && (!sheetSentOnce || now - lastSheetTime >= sheetIntervalMs)) {
    lastSheetTime = now;
    sheetSentOnce = true;

    WiFiClientSecure gsClient;
    gsClient.setInsecure();
    HTTPClient gsHttp;
    gsHttp.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    gsHttp.begin(gsClient, GOOGLE_SCRIPT_URL);
    gsHttp.addHeader("Content-Type", "application/json");
    gsHttp.setTimeout(8000);

    int gsCode = gsHttp.POST(payload);
    if (gsCode == 200 || gsCode == 302) {
      Serial.printf("POST Google Sheet -> OK (HTTP %d)\n", gsCode);
    } else if (gsCode > 0) {
      Serial.printf("POST Google Sheet -> HTTP %d: %s\n", gsCode, gsHttp.getString().c_str());
    } else {
      Serial.printf("POST Google Sheet failed: %s\n", gsHttp.errorToString(gsCode).c_str());
    }
    gsHttp.end();
  }
}

void fetchRemoteConfig() {
  if (WiFi.status() != WL_CONNECTED) return;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.begin(client, FIREBASE_CONFIG_URL);
  http.setTimeout(5000);

  int code = http.GET();
  if (code == 200) {
    String body = http.getString();
    body.trim();
    if (body == "null" || body.length() == 0) {
      JsonDocument defDoc;
      defDoc["read_interval_sec"]      = sendIntervalMs / 1000;
      defDoc["sheet_interval_sec"]     = sheetIntervalMs / 1000;
      defDoc["night_eco_enabled"]      = nightEcoEnabled;
      defDoc["night_eco_interval_sec"] = nightEcoIntervalMs / 1000;
      defDoc["lcd_mode"]               = lcdMode;
      defDoc["power_mode"]             = powerMode;
      String defPayload;
      serializeJson(defDoc, defPayload);
      http.end();

      HTTPClient putHttp;
      putHttp.begin(client, FIREBASE_CONFIG_URL);
      putHttp.addHeader("Content-Type", "application/json");
      putHttp.PUT(defPayload);
      putHttp.end();
      Serial.println("Seeded default /weather_config/weather-01 in Firebase");
      return;
    }

    JsonDocument cfg;
    if (deserializeJson(cfg, body) == DeserializationError::Ok) {
      if (cfg["read_interval_sec"].is<int>()) {
        int sec = max(5, cfg["read_interval_sec"].as<int>());
        sendIntervalMs = (unsigned long)sec * 1000UL;
      }
      if (cfg["sheet_interval_sec"].is<int>()) {
        int sec = max(30, cfg["sheet_interval_sec"].as<int>());
        sheetIntervalMs = (unsigned long)sec * 1000UL;
      }
      if (cfg["night_eco_enabled"].is<bool>()) {
        nightEcoEnabled = cfg["night_eco_enabled"].as<bool>();
      }
      if (cfg["night_eco_interval_sec"].is<int>()) {
        int sec = max(60, cfg["night_eco_interval_sec"].as<int>());
        nightEcoIntervalMs = (unsigned long)sec * 1000UL;
      }
      if (cfg["lcd_mode"].is<const char*>()) {
        lcdMode = cfg["lcd_mode"].as<String>();
      } else if (cfg["lcd_backlight"].is<bool>()) {
        lcdMode = cfg["lcd_backlight"].as<bool>() ? "on" : "text_only";
      }
      if (cfg["power_mode"].is<const char*>()) {
        powerMode = cfg["power_mode"].as<String>();
      } else if (cfg["deep_sleep_enabled"].is<bool>()) {
        powerMode = cfg["deep_sleep_enabled"].as<bool>() ? "deep_sleep" : "awake";
      }
      prefs.begin("weather_cfg", false);
      prefs.putString("lcd_mode", lcdMode);
      prefs.putString("power_mode", powerMode);
      prefs.end();

      applyLcdMode(false);
      applyPowerMode();
    }
  }
  http.end();
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== ESP32 Weather Station Starting ===");

  prefs.begin("weather_cfg", true);
  lcdMode   = prefs.getString("lcd_mode", lcdMode);
  powerMode = prefs.getString("power_mode", powerMode);
  prefs.end();

  if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0) {
    lcdWakeUntilMs = millis() + 30000UL;
  }

  pinMode(BOOT_BTN_PIN, INPUT_PULLUP);
  analogReadResolution(12);
  Wire.begin(21, 22);

  lcd.init();
  applyLcdMode(false);
  if (powerMode != "deep_sleep" && lcdMode != "off") {
    lcd.setCursor(0, 0);
    lcd.print("KH ClimateSense");
    lcd.setCursor(0, 1);
    lcd.print("Connecting WiFi");
  }

  if (ENABLE_DHT22) {
    dht.begin();
  }
  if (ENABLE_BH1750) {
    if (!lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE)) {
      Serial.println("Warning: BH1750 not found! Check SDA=21, SCL=22, ADDR=GND");
    }
  }

  connectWiFi();
  configTime(8 * 3600, 0, "pool.ntp.org", "time.nist.gov");
  fetchRemoteConfig();
}

void loop() {
  unsigned long now = millis();

  // Pressing BOOT button wakes LCD for 30s in headless/off/deep_sleep modes
  if (digitalRead(BOOT_BTN_PIN) == LOW) {
    lcdWakeUntilMs = now + 30000UL;
    applyLcdMode(false);
  }

  if (!sentOnBoot || now - lastLcdUpdate >= LCD_INTERVAL_MS) {
    lastLcdUpdate = now;
    SensorReading r = readSensors();
    if ((powerMode != "deep_sleep" && lcdMode != "off") || now < lcdWakeUntilMs) {
      updateLcd(r);
    }
    applyLcdMode(false);

    unsigned long effectiveMs = getEffectiveIntervalMs();
    if (!sentOnBoot || now - lastSendTime >= effectiveMs) {
      sentOnBoot = true;
      lastSendTime = now;
      sendTelemetry(r);
      fetchRemoteConfig();

      if (powerMode == "deep_sleep" && millis() >= lcdWakeUntilMs) {
        uint64_t sleepSec = max(5UL, getEffectiveIntervalMs() / 1000UL);
        Serial.printf("Entering Deep Sleep for %llu s...\n", sleepSec);
        Serial.flush();
        esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0);
        esp_sleep_enable_timer_wakeup(sleepSec * 1000000ULL);
        esp_deep_sleep_start();
      }
    }
  }
}
