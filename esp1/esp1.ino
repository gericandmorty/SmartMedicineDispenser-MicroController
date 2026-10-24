#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>

// ==============================================================================
//  SMART MEDICINE DISPENSER - ESP1 FIRMWARE (DRAWER 1: 21 DISCRETE LEDs)
//  Version: 4.0 (Exact 3-Row x 7-Column Cell Matrix matching Mobile App)
//
//  Mobile App Layout Alignment:
//    Columns: A, B, C, D, E, F, G (7 columns)
//    Rows:    1, 2, 3             (3 rows)
//
//    Row 1 (Top):    A1(4)   B1(5)   C1(6)   D1(7)   E1(8)   F1(9)   G1(10)  -> Slots 0..6
//    Row 2 (Middle): A2(11)  B2(12)  C2(13)  D2(14)  E2(15)  F2(16)  G2(17)  -> Slots 7..13
//    Row 3 (Bottom): A3(18)  B3(21)  C3(38)  D3(39)  E3(40)  F3(41)  G3(42)  -> Slots 14..20
//
//  Pin Verification:
//    - GPIO 4  = A1 (Slot 0)
//    - GPIO 5  = B1 (Slot 1)
//    - GPIO 38 = C3 (Slot 16)
//
//  Features:
//    1. 1-to-1 exact cell mapping: scheduling A1 only lits A1; scheduling C3 only lits C3!
//    2. When scheduled: that compartment LED stays SOLID ON.
//    3. When schedule is up: that compartment LED BLINKS rapidly (350ms interval).
//    4. ESP32-Mini simultaneously vibrates on GPIO 4 during the alert.
//    5. Non-blocking engine using millis() - Wi-Fi stays fast & responsive.
// ==============================================================================

// ── Wi-Fi Configuration ──────────────────────────────────────────────────────
const char* ssid     = "Hi";            // <-- 2.4G Wi-Fi SSID
const char* password = "estogs000";     // <-- 2.4G Wi-Fi password
const char* hostName = "dispenser-drawer1"; // http://dispenser-drawer1.local

// ── 3x7 LED Pin Mapping ──────────────────────────────────────────────────────
// Row 1: A1..G1
// Row 2: A2..G2
// Row 3: A3..G3
const int ledPins[3][7] = {
  {4,  5,  6,  7,  8,  9,  10}, // Row 1: A1(4), B1(5), C1(6), D1(7), E1(8), F1(9), G1(10)
  {11, 12, 13, 14, 15, 16, 17}, // Row 2: A2(11), B2(12), C2(13), D2(14), E2(15), F2(16), G2(17)
  {18, 21, 38, 39, 40, 41, 42}  // Row 3: A3(18), B3(21), C3(38), D3(39), E3(40), F3(41), G3(42)
};

// ── Compartment Labels Helper ────────────────────────────────────────────────
const char* slotLabels[21] = {
  "A1", "B1", "C1", "D1", "E1", "F1", "G1",
  "A2", "B2", "C2", "D2", "E2", "F2", "G2",
  "A3", "B3", "C3", "D3", "E3", "F3", "G3"
};

// ── 21 Individual Slot States ────────────────────────────────────────────────
bool slotScheduled[21] = {false}; // true = Solid ON (Active Schedule)
bool slotAlerting[21]  = {false}; // true = Blinking (Alarm is due now)

// Non-blocking blink timing
unsigned long lastBlinkMs = 0;
bool blinkState = false;
const unsigned long BLINK_INTERVAL = 350; // ms

WebServer server(80);

// ── Coordinate Converter ─────────────────────────────────────────────────────
// Returns slot index 0..20, or -1 if invalid
int parseSlotIndex(const String& input) {
  String s = input;
  s.trim();
  s.toUpperCase();

  // Strip parentheses e.g. "Drawer 1 (C3)" -> "C3"
  int p1 = s.indexOf('(');
  if (p1 != -1) {
    int p2 = s.indexOf(')', p1);
    if (p2 != -1) s = s.substring(p1 + 1, p2);
  }
  s.trim();

  // If passed directly as compartment label e.g. "A1", "C3"
  for (int i = 0; i < s.length(); i++) {
    char c = s.charAt(i);
    if (c >= 'A' && c <= 'G') {
      if (i + 1 < s.length()) {
        char r = s.charAt(i + 1);
        if (r >= '1' && r <= '3') {
          int col = c - 'A';      // 0..6
          int row = r - '1';      // 0..2
          return row * 7 + col;   // 0..20
        }
      }
    }
  }

  // If passed as a numeric string e.g. "0".."20"
  if (s.length() > 0) {
    bool isNum = true;
    for (int i = 0; i < s.length(); i++) {
      if (!isDigit(s.charAt(i))) { isNum = false; break; }
    }
    if (isNum) {
      int val = s.toInt();
      if (val >= 0 && val < 21) return val;
    }
  }

  return -1;
}

// ── Hardware LED Control ─────────────────────────────────────────────────────
void setSlotLED(int slot, bool state) {
  if (slot < 0 || slot >= 21) return;
  int row = slot / 7;
  int col = slot % 7;
  digitalWrite(ledPins[row][col], state ? HIGH : LOW);
}

void allOn() {
  for (int r = 0; r < 3; r++) {
    for (int c = 0; c < 7; c++) {
      digitalWrite(ledPins[r][c], HIGH);
    }
  }
}

void allOff() {
  for (int r = 0; r < 3; r++) {
    for (int c = 0; c < 7; c++) {
      digitalWrite(ledPins[r][c], LOW);
    }
  }
}

// ── Startup Visual Sweep Animation ───────────────────────────────────────────
void runStartupAnimation() {
  Serial.println("[ESP1] Running LED matrix verification (Row by Row, Col by Col)...");
  
  // Row 1 sweep
  for (int c = 0; c < 7; c++) {
    allOff();
    digitalWrite(ledPins[0][c], HIGH);
    delay(70);
  }
  // Row 2 sweep
  for (int c = 0; c < 7; c++) {
    allOff();
    digitalWrite(ledPins[1][c], HIGH);
    delay(70);
  }
  // Row 3 sweep
  for (int c = 0; c < 7; c++) {
    allOff();
    digitalWrite(ledPins[2][c], HIGH);
    delay(70);
  }

  // Quick double flash of all 21 LEDs
  allOn();
  delay(150);
  allOff();
  delay(100);
  allOn();
  delay(150);
  allOff();

  Serial.println("[ESP1] Matrix verification complete.");
}

// ── State Mutators ───────────────────────────────────────────────────────────
void setScheduleSlot(int slot, bool active = true) {
  if (slot < 0 || slot >= 21) return;
  slotScheduled[slot] = active;
  if (!active) {
    slotAlerting[slot] = false;
  }
  int r = slot / 7;
  int c = slot % 7;
  Serial.printf("[ESP1] Compartment %s (Slot %d, GPIO %d) Schedule: %s\n",
                slotLabels[slot], slot, ledPins[r][c], active ? "LIT (SOLID ON)" : "OFF");
}

void setAlertSlot(int slot, bool alerting = true) {
  if (slot < 0 || slot >= 21) return;
  slotAlerting[slot] = alerting;
  if (alerting) {
    slotScheduled[slot] = true; // Ensure it stays active
    int r = slot / 7;
    int c = slot % 7;
    Serial.printf("[ESP1] 🚨 ALARM UP! Compartment %s (Slot %d, GPIO %d) is BLINKING!\n",
                  slotLabels[slot], slot, ledPins[r][c]);
  } else {
    Serial.printf("[ESP1] ⏹ Alert cleared for Compartment %s (Slot %d)\n", slotLabels[slot], slot);
  }
}

void clearAll() {
  for (int i = 0; i < 21; i++) {
    slotScheduled[i] = false;
    slotAlerting[i] = false;
  }
  allOff();
  Serial.println("[ESP1] All 21 compartments cleared.");
}

// ── Non-Blocking LED Engine ──────────────────────────────────────────────────
void updateLEDState() {
  unsigned long now = millis();
  if (now - lastBlinkMs >= BLINK_INTERVAL) {
    lastBlinkMs = now;
    blinkState = !blinkState;
  }

  for (int slot = 0; slot < 21; slot++) {
    int r = slot / 7;
    int c = slot % 7;

    bool shouldBeOn = false;
    if (slotAlerting[slot]) {
      // Blinking alert when alarm is due
      shouldBeOn = blinkState;
    } else if (slotScheduled[slot]) {
      // Solid ON when scheduled in mobile app
      shouldBeOn = true;
    }

    digitalWrite(ledPins[r][c], shouldBeOn ? HIGH : LOW);
  }
}

// ── HTTP CORS Headers ────────────────────────────────────────────────────────
void sendCORSHeaders() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
}

// ── HTTP Request Handlers ────────────────────────────────────────────────────
void handleOptions() {
  sendCORSHeaders();
  server.send(204);
}

// GET/POST /schedule?slot=0 or ?comp=A1 or ?col=0&row=1
void handleSchedule() {
  sendCORSHeaders();
  int slot = -1;

  if (server.hasArg("slot")) {
    slot = server.arg("slot").toInt();
  } else if (server.hasArg("comp")) {
    slot = parseSlotIndex(server.arg("comp"));
  } else if (server.hasArg("col") && server.hasArg("row")) {
    int col = server.arg("col").toInt();
    int row = server.arg("row").toInt() - 1; // row 1..3 -> 0..2
    if (row >= 0 && row < 3 && col >= 0 && col < 7) {
      slot = row * 7 + col;
    }
  } else if (server.hasArg("col")) {
    // If only column provided, schedule all 3 rows in that column
    int col = server.arg("col").toInt();
    if (col >= 0 && col < 7) {
      for (int r = 0; r < 3; r++) setScheduleSlot(r * 7 + col, true);
      server.send(200, "application/json", "{\"status\":\"ok\",\"action\":\"column_scheduled\",\"col\":" + String(col) + "}");
      return;
    }
  }

  if (slot >= 0 && slot < 21) {
    setScheduleSlot(slot, true);
    String resp = "{\"status\":\"ok\",\"slot\":" + String(slot) + ",\"compartment\":\"" + String(slotLabels[slot]) + "\",\"gpio\":" + String(ledPins[slot/7][slot%7]) + ",\"state\":\"SOLID_ON\"}";
    server.send(200, "application/json", resp);
  } else {
    server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Provide valid slot (0..20) or comp (A1..G3)\"}");
  }
}

// GET/POST /alert?slot=0 or ?comp=A1 (Called by ESP32-Mini when alarm is due!)
void handleAlert() {
  sendCORSHeaders();
  int slot = -1;

  if (server.hasArg("slot")) {
    slot = server.arg("slot").toInt();
  } else if (server.hasArg("comp")) {
    slot = parseSlotIndex(server.arg("comp"));
  } else if (server.hasArg("col") && server.hasArg("row")) {
    int col = server.arg("col").toInt();
    int row = server.arg("row").toInt() - 1;
    if (row >= 0 && row < 3 && col >= 0 && col < 7) {
      slot = row * 7 + col;
    }
  }

  if (slot >= 0 && slot < 21) {
    setAlertSlot(slot, true);
    String resp = "{\"status\":\"ok\",\"action\":\"alert_started\",\"slot\":" + String(slot) + ",\"compartment\":\"" + String(slotLabels[slot]) + "\",\"mode\":\"BLINKING\"}";
    server.send(200, "application/json", resp);
  } else {
    server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Missing slot or comp\"}");
  }
}

// GET/POST /clear-alert?slot=0 or /stop-alert
void handleClearAlert() {
  sendCORSHeaders();
  if (server.hasArg("slot")) {
    int slot = server.arg("slot").toInt();
    setAlertSlot(slot, false);
  } else if (server.hasArg("comp")) {
    int slot = parseSlotIndex(server.arg("comp"));
    if (slot >= 0) setAlertSlot(slot, false);
  } else {
    // Clear all alerts
    for (int i = 0; i < 21; i++) slotAlerting[i] = false;
    Serial.println("[ESP1] Cleared all active alerts.");
  }
  server.send(200, "application/json", "{\"status\":\"ok\",\"action\":\"alert_cleared\"}");
}

// GET/POST /led?slot=0&state=active (Contract called by ESP32-Mini master)
void handleLed() {
  sendCORSHeaders();
  int slot = -1;

  if (server.hasArg("slot")) {
    slot = server.arg("slot").toInt();
  } else if (server.hasArg("comp")) {
    slot = parseSlotIndex(server.arg("comp"));
  }

  String state = server.hasArg("state") ? server.arg("state") : "active";

  if (slot >= 0 && slot < 21) {
    if (state.equalsIgnoreCase("active") || state.equalsIgnoreCase("on") || state.equalsIgnoreCase("green")) {
      setScheduleSlot(slot, true);
    } else if (state.equalsIgnoreCase("alert") || state.equalsIgnoreCase("blink")) {
      setAlertSlot(slot, true);
    } else if (state.equalsIgnoreCase("off") || state.equalsIgnoreCase("inactive")) {
      setScheduleSlot(slot, false);
    }
    server.send(200, "application/json", "{\"status\":\"ok\",\"slot\":" + String(slot) + ",\"compartment\":\"" + String(slotLabels[slot]) + "\",\"state\":\"" + state + "\"}");
  } else {
    server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Invalid slot or comp\"}");
  }
}

// GET /clear (Turns all off)
void handleClear() {
  sendCORSHeaders();
  clearAll();
  server.send(200, "application/json", "{\"status\":\"ok\",\"message\":\"All LEDs cleared\"}");
}

// GET /test (Plays matrix test)
void handleTest() {
  sendCORSHeaders();
  runStartupAnimation();
  server.send(200, "application/json", "{\"status\":\"ok\",\"message\":\"Matrix test animation executed\"}");
}

// GET /status
void handleStatus() {
  sendCORSHeaders();
  String json = "{";
  json += "\"status\":\"online\",";
  json += "\"device\":\"ESP1-Drawer1-DiscreteMatrix\",";
  json += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
  json += "\"rssi\":" + String(WiFi.RSSI()) + ",";
  
  json += "\"slots\":[";
  for (int i = 0; i < 21; i++) {
    if (i > 0) json += ",";
    json += "{\"slot\":" + String(i) + ",\"comp\":\"" + String(slotLabels[i]) + "\",";
    json += "\"gpio\":" + String(ledPins[i/7][i%7]) + ",";
    json += "\"scheduled\":" + String(slotScheduled[i] ? "true" : "false") + ",";
    json += "\"alerting\":" + String(slotAlerting[i] ? "true" : "false") + "}";
  }
  json += "]}";
  server.send(200, "application/json", json);
}

// GET / (Visual Dashboard matching Mobile App Layout)
void handleRoot() {
  sendCORSHeaders();
  String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1.0'>";
  html += "<title>ESP1 - Drawer 1 Compartments</title>";
  html += "<style>body{font-family:sans-serif;background:#18181B;color:#F4F4F5;padding:16px;text-align:center;}";
  html += ".grid-wrap{max-width:480px;margin:0 auto;background:#27272A;padding:16px;border-radius:16px;box-shadow:0 4px 12px rgba(0,0,0,0.5);}";
  html += ".row-header{display:grid;grid-template-columns:30px repeat(7, 1fr);gap:6px;font-weight:700;color:#22C55E;margin-bottom:8px;}";
  html += ".grid-row{display:grid;grid-template-columns:30px repeat(7, 1fr);gap:6px;align-items:center;margin-bottom:8px;}";
  html += ".row-num{color:#22C55E;font-weight:700;}";
  html += ".cell{background:#3F3F46;border-radius:8px;padding:8px 2px;font-size:11px;font-weight:700;text-decoration:none;display:block;color:#D4D4D8;border:2px solid transparent;}";
  html += ".cell-on{background:#064E3B;color:#6EE7B7;border-color:#10B981;box-shadow:0 0 8px #10B981;}";
  html += ".cell-alert{background:#7C2D12;color:#FDBA74;border-color:#F97316;box-shadow:0 0 12px #F97316;animation:blink 0.7s infinite alternate;}";
  html += "@keyframes blink{from{opacity:1;}to{opacity:0.3;}}";
  html += ".btn{display:inline-block;padding:8px 14px;border-radius:8px;text-decoration:none;font-weight:700;margin:6px;font-size:13px;}";
  html += ".btn-blue{background:#2563EB;color:#fff;} .btn-red{background:#DC2626;color:#fff;} .btn-orange{background:#EA580C;color:#fff;}";
  html += "</style></head><body>";
  html += "<h2>💊 Drawer 1 Matrix (21 Compartments)</h2>";
  html += "<p>IP: <strong>" + WiFi.localIP().toString() + "</strong> | Wi-Fi: <strong>" + String(ssid) + "</strong></p>";

  html += "<div class='grid-wrap'>";
  // Column letters: A B C D E F G
  html += "<div class='row-header'><div></div>";
  for (int c = 0; c < 7; c++) {
    html += "<div>" + String((char)('A' + c)) + "</div>";
  }
  html += "</div>";

  // Rows 1, 2, 3
  for (int r = 0; r < 3; r++) {
    html += "<div class='grid-row'>";
    html += "<div class='row-num'>" + String(r + 1) + "</div>";
    for (int c = 0; c < 7; c++) {
      int slot = r * 7 + c;
      String cls = "cell";
      if (slotAlerting[slot]) cls += " cell-alert";
      else if (slotScheduled[slot]) cls += " cell-on";
      html += "<a href='/led?slot=" + String(slot) + "&state=" + (slotScheduled[slot] ? "off" : "active") + "' class='" + cls + "'>";
      html += String(slotLabels[slot]) + "<br><small style='font-size:9px;opacity:0.8;'>G" + String(ledPins[r][c]) + "</small>";
      html += "</a>";
    }
    html += "</div>";
  }
  html += "</div>";

  html += "<div style='margin-top:16px;'>";
  html += "<a href='/test' class='btn btn-blue'>✨ Test Matrix Sweep</a>";
  html += "<a href='/clear-alert' class='btn btn-orange'>⏹ Silence Alert</a>";
  html += "<a href='/clear' class='btn btn-red'>🧹 Clear All LEDs</a>";
  html += "</div>";
  html += "<p style='font-size:12px;color:#A1A1AA;margin-top:8px;'>Tip: Tap any cell above to toggle its LED solid ON/OFF.</p>";
  html += "</body></html>";
  server.send(200, "text/html", html);
}

// ── Serial Command Handler ───────────────────────────────────────────────────
void handleSerialCommands() {
  if (Serial.available() > 0) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    if (cmd.length() == 0) return;

    Serial.printf("[ESP1 <- SERIAL] %s\n", cmd.c_str());

    // Format from ESP32-Mini: LED:1:<slot>:<action> (e.g. LED:1:16:alert or LED:1:0:active)
    if (cmd.startsWith("LED:")) {
      int firstColon = cmd.indexOf(':');
      int secondColon = cmd.indexOf(':', firstColon + 1);
      int thirdColon = cmd.indexOf(':', secondColon + 1);

      if (secondColon != -1 && thirdColon != -1) {
        int slot = cmd.substring(secondColon + 1, thirdColon).toInt();
        String action = cmd.substring(thirdColon + 1);

        if (action.equalsIgnoreCase("alert")) {
          setAlertSlot(slot, true);
        } else if (action.equalsIgnoreCase("clear-alert") || action.equalsIgnoreCase("off")) {
          setAlertSlot(slot, false);
          if (action.equalsIgnoreCase("off")) setScheduleSlot(slot, false);
        } else if (action.indexOf("active") != -1 || action.indexOf("green") != -1) {
          setScheduleSlot(slot, true);
        }
      }
    } else if (cmd.startsWith("ALERT:")) {
      String arg = cmd.substring(6);
      int slot = parseSlotIndex(arg);
      if (slot >= 0) setAlertSlot(slot, true);
    } else if (cmd.startsWith("SCHEDULE:")) {
      String arg = cmd.substring(9);
      int slot = parseSlotIndex(arg);
      if (slot >= 0) setScheduleSlot(slot, true);
    } else if (cmd.equalsIgnoreCase("STOP") || cmd.equalsIgnoreCase("CLEAR-ALERT")) {
      for (int i = 0; i < 21; i++) slotAlerting[i] = false;
      Serial.println("[ESP1] Cleared alerts.");
    } else if (cmd.equalsIgnoreCase("CLEAR")) {
      clearAll();
    } else if (cmd.equalsIgnoreCase("TEST")) {
      runStartupAnimation();
    } else if (cmd.equalsIgnoreCase("STATUS") || cmd.equalsIgnoreCase("IP")) {
      Serial.printf("[ESP1] IP: %s | Wi-Fi: %s (RSSI: %d dBm)\n",
                    WiFi.localIP().toString().c_str(), ssid, WiFi.RSSI());
    }
  }
}

// ── Setup ────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n========================================================");
  Serial.println("  SMART MEDICINE DISPENSER - ESP1 (DRAWER 1 LED MATRIX) ");
  Serial.println("  Exact 3x7 Grid: A1(GPIO 4) ... C3(GPIO 38) ... G3(42)");
  Serial.println("========================================================");

  // Initialize all 21 LED GPIO pins as OUTPUT and LOW
  for (int r = 0; r < 3; r++) {
    for (int c = 0; c < 7; c++) {
      pinMode(ledPins[r][c], OUTPUT);
      digitalWrite(ledPins[r][c], LOW);
    }
  }

  // Connect to Wi-Fi
  Serial.printf("[WIFI] Connecting to '%s'...\n", ssid);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false); // Keeps radio active for instant ping & response
  WiFi.begin(ssid, password);

  int retries = 0;
  while (WiFi.status() != WL_CONNECTED && retries < 30) {
    delay(500);
    Serial.print(".");
    retries++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[WIFI] Connected!");
    Serial.printf("[WIFI] Drawer 1 IP Address: %s\n", WiFi.localIP().toString().c_str());
    Serial.printf("[WIFI] Gateway: %s | RSSI: %d dBm\n", WiFi.gatewayIP().toString().c_str(), WiFi.RSSI());

    if (MDNS.begin(hostName)) {
      Serial.printf("[MDNS] Started: http://%s.local\n", hostName);
    }
  } else {
    Serial.println("\n[WIFI] ⚠️ Wi-Fi not connected yet. Will retry in background.");
  }

  // Setup Web Server Endpoints
  server.on("/", HTTP_GET, handleRoot);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/ping", HTTP_GET, handleStatus);
  server.on("/schedule", HTTP_GET, handleSchedule);
  server.on("/schedule", HTTP_POST, handleSchedule);
  server.on("/led", HTTP_GET, handleLed);
  server.on("/alert", HTTP_GET, handleAlert);
  server.on("/alert", HTTP_POST, handleAlert);
  server.on("/clear-alert", HTTP_GET, handleClearAlert);
  server.on("/clear-alert", HTTP_POST, handleClearAlert);
  server.on("/clear", HTTP_GET, handleClear);
  server.on("/test", HTTP_GET, handleTest);

  server.on("/schedule", HTTP_OPTIONS, handleOptions);
  server.on("/alert", HTTP_OPTIONS, handleOptions);
  server.on("/clear-alert", HTTP_OPTIONS, handleOptions);
  server.on("/led", HTTP_OPTIONS, handleOptions);

  server.begin();
  Serial.println("[HTTP] Server listening on port 80");

  // Visual startup verification sweep
  runStartupAnimation();
}

// ── Loop ─────────────────────────────────────────────────────────────────────
void loop() {
  server.handleClient();
  updateLEDState();
  handleSerialCommands();

  // Reconnect Wi-Fi if connection drops
  static unsigned long lastReconnectCheck = 0;
  if (WiFi.status() != WL_CONNECTED && (millis() - lastReconnectCheck >= 10000)) {
    lastReconnectCheck = millis();
    Serial.println("[WIFI] Reconnecting...");
    WiFi.reconnect();
  }

  // Heartbeat every 10 seconds to Serial
  static unsigned long lastHeartbeat = 0;
  if (millis() - lastHeartbeat >= 10000) {
    lastHeartbeat = millis();
    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("[ESP1 HEARTBEAT] 🟢 IP: %s | http://%s.local\n",
                    WiFi.localIP().toString().c_str(), hostName);
    }
  }
}
