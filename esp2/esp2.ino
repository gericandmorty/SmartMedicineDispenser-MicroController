// ==============================================================================
//  SMART MEDICINE DISPENSER - ESP2 FIRMWARE (DRAWER 2: 21 LEDs CONTROLLER)
// ==============================================================================
//  Hardware Target: ESP32 or ESP8266 (NodeMCU / Wemos D1 Mini / SuperMini)
//  Purpose:
//    Controls the 21 compartment LEDs for Drawer 2 (Compartments A1..G3).
//    - Slot 0..20 corresponds to:
//        Row 1: A1 (0), B1 (1), C1 (2), D1 (3), E1 (4), F1 (5), G1 (6)
//        Row 2: A2 (7), B2 (8), C2 (9), D2 (10), E2 (11), F2 (12), G2 (13)
//        Row 3: A3 (14), B3 (15), C3 (16), D3 (17), E3 (18), F3 (19), G3 (20)
//    - Also accepts global slot indices 21..41 (automatically offset by -21).
//
//  Color Schemes:
//    - GREEN  (0, 255, 0) : Normal active schedule (in stock)
//    - RED    (255, 0, 0) : Out of stock (0/7 doses remaining)
//    - PULSING/BLINKING WHITE/ORANGE : Active Alarm (due now within 20m window)
//    - OFF    (0, 0, 0)   : Inactive / empty compartment
//
//  Communication:
//    - Wi-Fi HTTP REST Server (Port 80)
//    - Serial commands (UART at 115200 baud)
// ==============================================================================

#if defined(ESP8266)
  #include <ESP8266WiFi.h>
  #include <ESP8266WebServer.h>
  #include <ESP8266mDNS.h>
  #define WebServerClass ESP8266WebServer
  #define LED_DATA_PIN  4  // D2 on NodeMCU / Wemos D1 Mini (GPIO 4)
#elif defined(ESP32)
  #include <WiFi.h>
  #include <WebServer.h>
  #include <ESPmDNS.h>
  #define WebServerClass WebServer
  #define LED_DATA_PIN  4  // GPIO 4 on ESP32
#else
  #error "Please select an ESP8266 or ESP32 board in Arduino IDE"
#endif

#include <Adafruit_NeoPixel.h>

// ── Wi-Fi Credentials ────────────────────────────────────────────────────────
const char* ssid     = "CIREG";            // <-- Set your Wi-Fi SSID
const char* password = "walongone...";       // <-- Set your Wi-Fi Password
const char* hostName = "dispenser-drawer2"; // mDNS: http://dispenser-drawer2.local

// ── LED Strip Configuration ──────────────────────────────────────────────────
#define NUM_LEDS      21        // 21 compartments in Drawer 2 (7 columns x 3 rows)
#define BRIGHTNESS    180       // 0 - 255
#define DRAWER_INDEX  2         // Drawer 2

// WS2812B / NeoPixel configuration (GRB bitstream at 800 KHz)
Adafruit_NeoPixel strip(NUM_LEDS, LED_DATA_PIN, NEO_GRB + NEO_KHZ800);

WebServerClass server(80);

// ── LED State Storage ────────────────────────────────────────────────────────
enum CompartmentState {
  STATE_OFF = 0,
  STATE_ACTIVE_GREEN,
  STATE_OUT_OF_STOCK_RED,
  STATE_ALERTING_BLINK
};

CompartmentState slotStates[NUM_LEDS];

// Non-blocking blinking state for alerting LEDs
bool blinkPhase = false;
unsigned long lastBlinkTime = 0;
const unsigned long BLINK_INTERVAL_MS = 350;

// ── Compartment String to Slot Index Mapper (e.g. "A1" -> 0, "G3" -> 20) ──────
int compartmentToSlot(const String& comp) {
  String c = comp;
  c.toUpperCase();
  c.trim();

  // Strip "DRAWER 2 (" or "(" if present
  int openParen = c.indexOf('(');
  if (openParen != -1) {
    int closeParen = c.indexOf(')');
    if (closeParen != -1) {
      c = c.substring(openParen + 1, closeParen);
    }
  }

  if (c.length() >= 2) {
    char col = c.charAt(0);
    char row = c.charAt(1);

    if (col >= 'A' && col <= 'G' && row >= '1' && row <= '3') {
      int colIdx = col - 'A';       // 0 to 6
      int rowIdx = row - '1';       // 0 to 2
      return rowIdx * 7 + colIdx;   // 0 to 20
    }
  }
  return -1;
}

// ── Set Hardware LED Output ──────────────────────────────────────────────────
void applySlotColor(int slot, CompartmentState state, bool blinkOn = true) {
  if (slot < 0 || slot >= NUM_LEDS) return;

  switch (state) {
    case STATE_OFF:
      strip.setPixelColor(slot, strip.Color(0, 0, 0));
      break;

    case STATE_ACTIVE_GREEN:
      strip.setPixelColor(slot, strip.Color(0, 220, 0)); // Botanical Green
      break;

    case STATE_OUT_OF_STOCK_RED:
      strip.setPixelColor(slot, strip.Color(255, 0, 0)); // Pure Warning Red
      break;

    case STATE_ALERTING_BLINK:
      if (blinkOn) {
        strip.setPixelColor(slot, strip.Color(255, 140, 0)); // Glowing Amber Alert
      } else {
        strip.setPixelColor(slot, strip.Color(40, 20, 0));  // Dim pulse
      }
      break;
  }
}

void renderAllLEDs() {
  for (int i = 0; i < NUM_LEDS; i++) {
    applySlotColor(i, slotStates[i], blinkPhase);
  }
  strip.show();
}

// ── Wi-Fi Connection Helper ──────────────────────────────────────────────────
void setupWiFi() {
  Serial.print("[ESP2] Connecting to Wi-Fi SSID: ");
  Serial.println(ssid);

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  int retries = 0;
  while (WiFi.status() != WL_CONNECTED && retries < 25) {
    delay(500);
    Serial.print(".");
    retries++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[ESP2] Wi-Fi Connected!");
    Serial.print("[ESP2] Drawer 2 IP Address: ");
    Serial.println(WiFi.localIP());

    if (MDNS.begin(hostName)) {
      Serial.printf("[ESP2] mDNS responder started: http://%s.local\n", hostName);
    }
  } else {
    Serial.println("\n[ESP2] Wi-Fi Connection failed. Running in Serial-only mode.");
  }
}

// ── CORS Headers for Web/Mobile Apps ─────────────────────────────────────────
void sendCORS() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
}

// Normalize slot: allows 0..20 or global 21..41
int normalizeSlot(int rawSlot) {
  if (rawSlot >= 21 && rawSlot <= 41) {
    return rawSlot - 21;
  }
  return rawSlot;
}

// ── HTTP API Handlers ────────────────────────────────────────────────────────

// GET /status
void handleStatus() {
  sendCORS();
  String json = "{";
  json += "\"drawer\":2,";
  json += "\"device\":\"ESP2-Drawer2-LEDs\",";
  json += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
  json += "\"led_count\":" + String(NUM_LEDS) + ",";
  json += "\"slots\":[";
  for (int i = 0; i < NUM_LEDS; i++) {
    if (i > 0) json += ",";
    json += String(slotStates[i]);
  }
  json += "]}";
  server.send(200, "application/json", json);
}

// GET /led?slot=0&state=active|out_of_stock|alert|off&color=green|red|orange|off
void handleSetLED() {
  sendCORS();

  int slot = -1;
  if (server.hasArg("slot")) {
    slot = normalizeSlot(server.arg("slot").toInt());
  } else if (server.hasArg("comp")) {
    slot = compartmentToSlot(server.arg("comp"));
  }

  if (slot < 0 || slot >= NUM_LEDS) {
    server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Invalid slot (0-20 or 21-41 required)\"}");
    return;
  }

  String stateArg = server.hasArg("state") ? server.arg("state") : "";
  String colorArg = server.hasArg("color") ? server.arg("color") : "";
  stateArg.toLowerCase();
  colorArg.toLowerCase();

  if (stateArg == "alert" || colorArg == "alert" || colorArg == "orange") {
    slotStates[slot] = STATE_ALERTING_BLINK;
  } else if (stateArg == "out_of_stock" || stateArg == "empty" || colorArg == "red") {
    slotStates[slot] = STATE_OUT_OF_STOCK_RED;
  } else if (stateArg == "active" || stateArg == "on" || colorArg == "green") {
    slotStates[slot] = STATE_ACTIVE_GREEN;
  } else if (stateArg == "off" || colorArg == "off") {
    slotStates[slot] = STATE_OFF;
  }

  renderAllLEDs();

  Serial.printf("[ESP2] Slot %d set to state %d\n", slot, slotStates[slot]);
  server.send(200, "application/json", "{\"status\":\"ok\",\"drawer\":2,\"slot\":" + String(slot) + ",\"state\":" + String(slotStates[slot]) + "}");
}

// GET /alert?slot=0 (Starts flashing amber alert)
void handleAlert() {
  sendCORS();
  int slot = -1;
  if (server.hasArg("slot")) {
    slot = normalizeSlot(server.arg("slot").toInt());
  } else if (server.hasArg("comp")) {
    slot = compartmentToSlot(server.arg("comp"));
  }

  if (slot >= 0 && slot < NUM_LEDS) {
    slotStates[slot] = STATE_ALERTING_BLINK;
    renderAllLEDs();
    server.send(200, "application/json", "{\"status\":\"ok\",\"action\":\"alerting\",\"slot\":" + String(slot) + "}");
  } else {
    server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Invalid slot\"}");
  }
}

// GET /clear-alert?slot=0 (Restores to active green or off)
void handleClearAlert() {
  sendCORS();
  int slot = -1;
  if (server.hasArg("slot")) {
    slot = normalizeSlot(server.arg("slot").toInt());
  } else if (server.hasArg("comp")) {
    slot = compartmentToSlot(server.arg("comp"));
  }

  if (slot >= 0 && slot < NUM_LEDS) {
    slotStates[slot] = STATE_ACTIVE_GREEN;
    renderAllLEDs();
    server.send(200, "application/json", "{\"status\":\"ok\",\"action\":\"cleared\",\"slot\":" + String(slot) + "}");
  } else {
    server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Invalid slot\"}");
  }
}

// GET /clear (Turn all 21 LEDs off)
void handleClearAll() {
  sendCORS();
  for (int i = 0; i < NUM_LEDS; i++) {
    slotStates[i] = STATE_OFF;
  }
  renderAllLEDs();
  Serial.println("[ESP2] All 21 LEDs cleared.");
  server.send(200, "application/json", "{\"status\":\"ok\",\"action\":\"all_cleared\"}");
}

// GET /test (Sequential sweep animation through all 21 LEDs)
void handleTestSweep() {
  sendCORS();
  server.send(200, "application/json", "{\"status\":\"ok\",\"action\":\"testing_sweep\"}");

  // Quick rainbow/sweep test
  for (int i = 0; i < NUM_LEDS; i++) {
    strip.setPixelColor(i, strip.Color(0, 255, 0));
    strip.show();
    delay(50);
  }
  delay(300);
  for (int i = 0; i < NUM_LEDS; i++) {
    strip.setPixelColor(i, strip.Color(255, 0, 0));
    strip.show();
    delay(50);
  }
  delay(300);
  renderAllLEDs();
}

// ── Setup ────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  Serial.println("\n[ESP2] Booting Drawer 2 (21 LEDs Controller)...");

  // Initialize NeoPixel Strip
  strip.begin();
  strip.setBrightness(BRIGHTNESS);
  strip.clear();
  strip.show();

  // Initialize all slots to OFF
  for (int i = 0; i < NUM_LEDS; i++) {
    slotStates[i] = STATE_OFF;
  }

  // Quick power-on self test (Flash green once)
  for (int i = 0; i < NUM_LEDS; i++) strip.setPixelColor(i, strip.Color(0, 100, 0));
  strip.show();
  delay(400);
  strip.clear();
  strip.show();

  setupWiFi();

  // Register Web Server Endpoints
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/led", HTTP_GET, handleSetLED);
  server.on("/alert", HTTP_GET, handleAlert);
  server.on("/clear-alert", HTTP_GET, handleClearAlert);
  server.on("/clear", HTTP_GET, handleClearAll);
  server.on("/test", HTTP_GET, handleTestSweep);

  server.begin();
  Serial.println("[ESP2] HTTP Server active on port 80");
}

// ── Loop ─────────────────────────────────────────────────────────────────────
void loop() {
  // 1. Handle HTTP Requests
  server.handleClient();

  // 2. Non-blocking blinking for any actively alerting compartments
  unsigned long now = millis();
  if (now - lastBlinkTime >= BLINK_INTERVAL_MS) {
    lastBlinkTime = now;
    blinkPhase = !blinkPhase;

    bool hasAlerting = false;
    for (int i = 0; i < NUM_LEDS; i++) {
      if (slotStates[i] == STATE_ALERTING_BLINK) {
        hasAlerting = true;
        break;
      }
    }

    if (hasAlerting) {
      renderAllLEDs();
    }
  }

  // 3. Serial Command Interface (Fallback / Direct Master UART)
  // Format: "LED:0:GREEN", "LED:0:RED", "ALERT:0", "CLEAR"
  if (Serial.available() > 0) {
    String line = Serial.readStringUntil('\n');
    line.trim();

    if (line == "CLEAR") {
      handleClearAll();
    } else if (line.startsWith("ALERT:")) {
      int s = normalizeSlot(line.substring(6).toInt());
      if (s >= 0 && s < NUM_LEDS) {
        slotStates[s] = STATE_ALERTING_BLINK;
        renderAllLEDs();
      }
    } else if (line.startsWith("LED:")) {
      int firstColon = line.indexOf(':');
      int secondColon = line.indexOf(':', firstColon + 1);
      if (firstColon != -1 && secondColon != -1) {
        int s = normalizeSlot(line.substring(firstColon + 1, secondColon).toInt());
        String col = line.substring(secondColon + 1);
        col.toUpperCase();
        if (s >= 0 && s < NUM_LEDS) {
          if (col == "GREEN") slotStates[s] = STATE_ACTIVE_GREEN;
          else if (col == "RED") slotStates[s] = STATE_OUT_OF_STOCK_RED;
          else if (col == "ALERT" || col == "ORANGE") slotStates[s] = STATE_ALERTING_BLINK;
          else slotStates[s] = STATE_OFF;
          renderAllLEDs();
        }
      }
    }
  }
}
