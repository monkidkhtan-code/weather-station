"""
Weather & Climate Station Backend Server
Runs with Python 3 standard library (no pip install required).

Endpoints:
  GET  /                  -> Web dashboard (server/static/index.html)
  POST /api/telemetry     -> Ingest JSON from weather-01 or moisture-01
  GET  /api/latest        -> Latest reading per device_id + irrigation hint
  GET  /api/history       -> Recent readings (?device_id=weather-01&limit=60)
"""

from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import sqlite3
import sys
from urllib.parse import parse_qs, urlparse

BASE_DIR = Path(__file__).resolve().parent
DB_PATH = BASE_DIR / "weather.db"
LOG_PATH = BASE_DIR / "server.log"
STATIC_INDEX = BASE_DIR / "static" / "index.html"
HOST = "0.0.0.0"
PORT = int(os.environ.get("PORT", "8000"))

# Ensure pythonw.exe (windowless mode) has valid stdout/stderr streams
if sys.stdout is None or sys.stderr is None:
    log_file = open(LOG_PATH, "a", encoding="utf-8", buffering=1)
    if sys.stdout is None:
        sys.stdout = log_file
    if sys.stderr is None:
        sys.stderr = log_file


def init_db() -> None:
    with sqlite3.connect(DB_PATH) as conn:
        conn.execute(
            """
            CREATE TABLE IF NOT EXISTS telemetry (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                received_at TEXT NOT NULL,
                device_id TEXT NOT NULL,
                device_type TEXT NOT NULL,
                payload_json TEXT NOT NULL
            )
            """
        )
        conn.execute(
            "CREATE INDEX IF NOT EXISTS idx_device_time ON telemetry (device_id, id DESC)"
        )
        conn.commit()


def insert_telemetry(device_id: str, device_type: str, payload: dict) -> str:
    now_iso = datetime.now(timezone.utc).isoformat()
    with sqlite3.connect(DB_PATH) as conn:
        conn.execute(
            "INSERT INTO telemetry (received_at, device_id, device_type, payload_json) VALUES (?, ?, ?, ?)",
            (now_iso, device_id, device_type, json.dumps(payload)),
        )
        conn.commit()
    return now_iso


def get_latest_by_device() -> dict:
    with sqlite3.connect(DB_PATH) as conn:
        conn.row_factory = sqlite3.Row
        rows = conn.execute(
            """
            SELECT t.device_id, t.device_type, t.received_at, t.payload_json
            FROM telemetry t
            INNER JOIN (
                SELECT device_id, MAX(id) AS max_id
                FROM telemetry
                GROUP BY device_id
            ) latest ON t.id = latest.max_id
            ORDER BY t.device_id
            """
        ).fetchall()

    devices = {}
    for row in rows:
        data = json.loads(row["payload_json"])
        data["received_at"] = row["received_at"]
        data["device_id"] = row["device_id"]
        data["device_type"] = row["device_type"]
        devices[row["device_id"]] = data
    return devices


def get_history(device_id: str, limit: int) -> list[dict]:
    with sqlite3.connect(DB_PATH) as conn:
        conn.row_factory = sqlite3.Row
        rows = conn.execute(
            """
            SELECT received_at, payload_json
            FROM telemetry
            WHERE device_id = ?
            ORDER BY id DESC
            LIMIT ?
            """,
            (device_id, limit),
        ).fetchall()

    items = []
    for row in reversed(rows):
        data = json.loads(row["payload_json"])
        data["received_at"] = row["received_at"]
        items.append(data)
    return items


def build_link_advice(devices: dict) -> dict:
    weather = next(
        (d for d in devices.values() if d.get("device_type") == "weather"), None
    )
    moisture = next(
        (d for d in devices.values() if d.get("device_type") == "moisture"), None
    )

    if not weather and not moisture:
        return {"status": "waiting", "message": "Waiting for weather-01 telemetry..."}
    if weather and not moisture:
        rain_note = "Raining now" if weather.get("is_raining") else "Dry weather"
        return {
            "status": "partial",
            "message": f"{rain_note}. Connect a moisture node (device_type: 'moisture') to enable smart watering advice.",
        }

    is_raining = bool(weather and weather.get("is_raining"))
    soil_pct = moisture.get("soil_moisture_pct")
    if is_raining:
        return {"status": "hold", "message": "Rain detected — skip watering."}
    if soil_pct is not None and soil_pct < 30:
        return {
            "status": "water",
            "message": f"Soil moisture low ({soil_pct}%) and no rain — watering recommended.",
        }
    return {
        "status": "ok",
        "message": f"Soil moisture healthy ({soil_pct}%).",
    }


class RequestHandler(BaseHTTPRequestHandler):
    def _send_json(self, status: int, body: dict | list) -> None:
        raw = json.dumps(body).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(raw)))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(raw)

    def _send_html(self, path: Path) -> None:
        if not path.exists():
            self._send_json(404, {"error": "index.html not found"})
            return
        raw = path.read_bytes()
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)

    def do_OPTIONS(self) -> None:
        self.send_response(204)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.end_headers()

    def do_GET(self) -> None:
        parsed = urlparse(self.path)
        if parsed.path == "/":
            self._send_html(STATIC_INDEX)
            return

        if parsed.path == "/api/latest":
            devices = get_latest_by_device()
            self._send_json(
                200,
                {
                    "devices": devices,
                    "advice": build_link_advice(devices),
                },
            )
            return

        if parsed.path == "/api/history":
            qs = parse_qs(parsed.query)
            device_id = qs.get("device_id", ["weather-01"])[0]
            try:
                limit = max(1, min(500, int(qs.get("limit", ["60"])[0])))
            except ValueError:
                limit = 60
            self._send_json(
                200,
                {"device_id": device_id, "items": get_history(device_id, limit)},
            )
            return

        self._send_json(404, {"error": "Not found"})

    def do_POST(self) -> None:
        parsed = urlparse(self.path)
        if parsed.path != "/api/telemetry":
            self._send_json(404, {"error": "Not found"})
            return

        length = int(self.headers.get("Content-Length", "0"))
        try:
            payload = json.loads(self.rfile.read(length).decode("utf-8"))
            if not isinstance(payload, dict):
                raise ValueError("Payload must be a JSON object")
        except Exception as exc:
            self._send_json(400, {"error": f"Invalid JSON: {exc}"})
            return

        device_id = str(payload.get("device_id") or "weather-01")
        device_type = str(payload.get("device_type") or "weather")
        received_at = insert_telemetry(device_id, device_type, payload)
        self._send_json(201, {"ok": True, "device_id": device_id, "received_at": received_at})


def main() -> None:
    init_db()
    server = ThreadingHTTPServer((HOST, PORT), RequestHandler)
    print(f"Weather Station Backend running at http://localhost:{PORT}")
    print(f"SQLite database: {DB_PATH}")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nShutting down...")
        server.server_close()


if __name__ == "__main__":
    main()
