/**
 * Google Apps Script for Weather & Climate Station (Standalone New Sheet)
 *
 * Setup:
 * 1. Create a brand new Google Sheet (e.g. "KH Weather Station Log").
 * 2. Go to Extensions -> Apps Script.
 * 3. Paste this entire file into Code.gs and click Save.
 * 4. Click Deploy -> New deployment -> Select type: Web app.
 *    - Execute as: Me
 *    - Who has access: Anyone
 * 5. Click Deploy, authorize permissions, and copy the Web App URL.
 * 6. Paste that URL into GOOGLE_SCRIPT_URL in weather_station.ino.
 */

const SHEET_NAME = "WeatherLog";

function doGet(e) {
  try {
    const ss = SpreadsheetApp.getActiveSpreadsheet();
    const sheet = ss.getSheetByName(SHEET_NAME);
    if (!sheet || sheet.getLastRow() <= 1) {
      return ContentService
        .createTextOutput(JSON.stringify({ ok: true, items: [] }))
        .setMimeType(ContentService.MimeType.JSON);
    }
    const lastRow = sheet.getLastRow();
    const limit = Math.min(2000, lastRow - 1);
    const startRow = lastRow - limit + 1;
    const rows = sheet.getRange(startRow, 1, limit, 10).getValues();
    const items = rows.map(r => ({
      ts: new Date(r[0]).getTime(),
      temp: r[2] === "" ? null : Number(r[2]),
      hum: r[3] === "" ? null : Number(r[3]),
      lux: r[4] === "" ? null : Number(r[4]),
      batV: r[8] === "" ? null : Number(r[8])
    })).filter(it => !isNaN(it.ts));

    return ContentService
      .createTextOutput(JSON.stringify({ ok: true, items }))
      .setMimeType(ContentService.MimeType.JSON);
  } catch (err) {
    return ContentService
      .createTextOutput(JSON.stringify({ ok: false, error: err.message }))
      .setMimeType(ContentService.MimeType.JSON);
  }
}

function doPost(e) {
  try {
    const data = JSON.parse(e.postData.contents);
    const ss = SpreadsheetApp.getActiveSpreadsheet();
    let sheet = ss.getSheetByName(SHEET_NAME);

    if (!sheet) {
      sheet = ss.insertSheet(SHEET_NAME);
    }

    if (sheet.getLastRow() === 0) {
      sheet.appendRow([
        "Timestamp",
        "Device ID",
        "Temp (C)",
        "Humidity (%)",
        "Light (Lux)",
        "Rain Raw",
        "Rain (%)",
        "Is Raining",
        "Battery (V)",
        "Battery (%)"
      ]);
      sheet.getRange(1, 1, 1, 10).setFontWeight("bold");
    }

    sheet.appendRow([
      new Date(),
      data.device_id || "weather-01",
      data.temp_c !== undefined ? data.temp_c : "",
      data.humidity !== undefined ? data.humidity : "",
      data.lux !== undefined ? data.lux : "",
      data.rain_raw !== undefined ? data.rain_raw : "",
      data.rain_pct !== undefined ? data.rain_pct : "",
      data.is_raining !== undefined ? data.is_raining : "",
      data.battery_v !== undefined ? data.battery_v : "",
      data.battery_pct !== undefined ? data.battery_pct : ""
    ]);

    return ContentService
      .createTextOutput(JSON.stringify({ ok: true }))
      .setMimeType(ContentService.MimeType.JSON);
  } catch (err) {
    return ContentService
      .createTextOutput(JSON.stringify({ ok: false, error: err.message }))
      .setMimeType(ContentService.MimeType.JSON);
  }
}
