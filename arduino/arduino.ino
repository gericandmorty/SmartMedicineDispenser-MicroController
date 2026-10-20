// ==============================================================================
//  SMART MEDICINE DISPENSER - ARDUINO CONTROLLER (LCD 16x2 & DISPENSE RELAYS)
// ==============================================================================
//  Hardware:
//    - Arduino UNO R4 WiFi, UNO R3, or Nano / Mega
//    - 16x2 I2C LCD Display (SDA/SCL at address 0x27)
//    - 4-Channel Active-LOW Relay Module (Pins 7, 8, 9, 10)
//
//  Function:
//    - Displays real-time status and alerts on the 16x2 LCD screen
//    - Controls physical dispensing solenoids/motors via relays
//    - Communicates with ESP32-Mini over Serial (UART) and optional Wi-Fi (UNO R4)
//
//  Serial Commands (From ESP32-Mini):
//    - "ALERT:Drawer 1 (A1)" -> Displays alert on LCD
//    - "DISPENSE:1"          -> Fires Relay 1 (2-second pulse) & updates LCD
//    - "TAKEN:Drawer 1 (A1)" -> Displays dose taken confirmation
//    - "MISSED:Drawer 1 (A1)"-> Displays missed dose alert
//    - "PING"                -> Replies "PONG:OK"
// ==============================================================================

#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// If compiling on Arduino UNO R4 WiFi, include WiFiS3.
// On standard Arduino Uno R3 / Nano / Mega, Wi-Fi is skipped and Serial is used.
#if defined(ARDUINO_UNOR4_WIFI)
  #include <WiFiS3.h>
  #define HAS_WIFI 1
#else
  #define HAS_WIFI 0
#endif

// ── Wi-Fi Configuration (for Uno R4 WiFi) ────────────────────────────────────
#if HAS_WIFI
  const char* ssid = "CIREG";
  const char* pass = "walongone...";
  WiFiServer server(80);
#endif

// Initialize 16x2 I2C LCD (Default I2C address 0x27)
LiquidCrystal_I2C lcd(0x27, 16, 2);

// Relay output pins
const int RELAY1 = 7;  // Relay 1 (Drawer 1 - A1)
const int RELAY2 = 8;  // Relay 2 (Drawer 1 - A2)
const int RELAY3 = 9;  // Relay 3 (Drawer 1 - B1)
const int RELAY4 = 10; // Relay 4 (Drawer 1 - B2)

// Active LOW relay module constants
const int RELAY_ON  = LOW;
const int RELAY_OFF = HIGH;

// Dispense pulse duration (2000 milliseconds = 2 seconds)
const unsigned long DISPENSE_DURATION = 2000;

// LCD Auto-reset timer
unsigned long lcdResetTime = 0;
bool lcdNeedsReset = false;

// ── Helper: Update 16x2 LCD Screen ───────────────────────────────────────────
void updateDisplay(String line1, String line2) {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(line1.substring(0, 16));
  lcd.setCursor(0, 1);
  lcd.print(line2.substring(0, 16));
}

void scheduleDefaultScreen(unsigned long delayMs = 3000) {
  lcdResetTime = millis() + delayMs;
  lcdNeedsReset = true;
}

// ── Dispense Compartment Relay ───────────────────────────────────────────────
void dispenseCompartment(int relayNum) {
  int pin = -1;
  String label = "";

  switch (relayNum) {
    case 1: pin = RELAY1; label = "Compartment 1"; break;
    case 2: pin = RELAY2; label = "Compartment 2"; break;
    case 3: pin = RELAY3; label = "Compartment 3"; break;
    case 4: pin = RELAY4; label = "Compartment 4"; break;
    default: pin = -1; break;
  }

  if (pin != -1) {
    Serial.print("ACK:DISPENSING:");
    Serial.println(relayNum);

    updateDisplay("DISPENSING...", label);
    digitalWrite(pin, RELAY_ON);
    delay(DISPENSE_DURATION);
    digitalWrite(pin, RELAY_OFF);

    updateDisplay("DISPENSED!", label);
    scheduleDefaultScreen(2500);
  } else {
    Serial.print("ERROR:INVALID_RELAY:");
    Serial.println(relayNum);
    updateDisplay("Error", "Invalid Relay");
    scheduleDefaultScreen(1500);
  }
}

// ── Setup ────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);

  // Initialize I2C LCD
  lcd.init();
  lcd.backlight();
  updateDisplay("Smart Dispenser", "Initializing...");

  // Initialize Relays (Active LOW)
  pinMode(RELAY1, OUTPUT); digitalWrite(RELAY1, RELAY_OFF);
  pinMode(RELAY2, OUTPUT); digitalWrite(RELAY2, RELAY_OFF);
  pinMode(RELAY3, OUTPUT); digitalWrite(RELAY3, RELAY_OFF);
  pinMode(RELAY4, OUTPUT); digitalWrite(RELAY4, RELAY_OFF);

  delay(1200);

#if HAS_WIFI
  updateDisplay("Connecting WiFi", String(ssid).substring(0, 16));
  WiFi.begin(ssid, pass);
  int retries = 0;
  while (WiFi.status() != WL_CONNECTED && retries < 20) {
    delay(500);
    retries++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    server.begin();
    IPAddress ip = WiFi.localIP();
    updateDisplay("WiFi Connected!", ip.toString());
    delay(2000);
  }
#endif

  updateDisplay("Smart Dispenser", "Waiting Schedule");
}

// ── Process Serial Commands from ESP32-Mini ──────────────────────────────────
void processSerialCommand(const String& cmd) {
  if (cmd.startsWith("DISPENSE:")) {
    int relayNum = cmd.substring(9).toInt();
    dispenseCompartment(relayNum);
  }
  else if (cmd.startsWith("ALERT:")) {
    String comp = cmd.substring(6);
    updateDisplay("TIME FOR MEDS!", comp);
  }
  else if (cmd.startsWith("TAKEN:")) {
    String comp = cmd.substring(6);
    updateDisplay("DOSE TAKEN! :)", comp);
    scheduleDefaultScreen(3000);
  }
  else if (cmd.startsWith("MISSED:")) {
    String comp = cmd.substring(7);
    updateDisplay("DOSE MISSED :(", comp);
    scheduleDefaultScreen(4000);
  }
  else if (cmd == "PING") {
    Serial.println("PONG:OK");
    updateDisplay("App Connected", "Ready");
    scheduleDefaultScreen(1500);
  }
}

// ── Loop ─────────────────────────────────────────────────────────────────────
void loop() {
  // 1. Check Serial input from ESP32-Mini
  if (Serial.available() > 0) {
    String command = Serial.readStringUntil('\n');
    command.trim();
    if (command.length() > 0) {
      processSerialCommand(command);
    }
  }

  // 2. Handle LCD Auto-Reset back to idle
  if (lcdNeedsReset && millis() >= lcdResetTime) {
    lcdNeedsReset = false;
    updateDisplay("Smart Dispenser", "Waiting Schedule");
  }

#if HAS_WIFI
  // 3. Handle Wi-Fi HTTP requests on UNO R4
  WiFiClient client = server.available();
  if (client) {
    String req = client.readStringUntil('\r');
    client.flush();

    if (req.indexOf("/dispense") != -1) {
      int rIdx = req.indexOf("relay=");
      int rNum = (rIdx != -1) ? req.substring(rIdx + 6, rIdx + 7).toInt() : 1;
      client.println("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n\r\n{\"status\":\"ok\"}");
      client.stop();
      dispenseCompartment(rNum);
    } else if (req.indexOf("/ping") != -1 || req.indexOf("/status") != -1) {
      client.println("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n\r\n{\"status\":\"online\"}");
      client.stop();
    } else {
      client.println("HTTP/1.1 404 Not Found\r\n\r\nNot Found");
      client.stop();
    }
  }
#endif
}