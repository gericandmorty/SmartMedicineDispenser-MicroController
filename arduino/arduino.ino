// ==============================================================================
//  SMART MEDICINE DISPENSER - ARDUINO CONTROLLER (UNO R3 / R4 WiFi)
// ==============================================================================
//  Hardware Setup:
//    1. I2C LCD 16x2 Display:
//       - SDA -> Arduino SDA (Pin A4 on Uno R3)
//       - SCL -> Arduino SCL (Pin A5 on Uno R3)
//       - Default I2C Address: 0x27 (or 0x3F)
//
//    2. Buzzer Relay Module (1 GPIO):
//       - Pin 7 -> Relay IN (Controls cabinet buzzer beep alert)
//
//    3. Solenoid Lock (1 GPIO):
//       - Pin 8 -> Solenoid Driver / Relay IN (Unlocks compartment door)
//
//    4. Magnetic Reed Switches (2 GPIOs):
//       - Pin 2 -> Drawer 1 Magnetic Reed Switch (INPUT_PULLUP, Pin 2 to GND)
//       - Pin 3 -> Drawer 2 Magnetic Reed Switch (INPUT_PULLUP, Pin 3 to GND)
//
//    5. Serial UART to ESP32-Mini Master (Wi-Fi Bridge):
//       - Pin 0 (RX) <- ESP32 TX
//       - Pin 1 (TX) -> ESP32 RX
//       - Baud Rate: 115200
//
//  Logic Flow (Per mobile_logic_verification_spec.md & System Flowchart):
//    1. Schedule arrives from Mobile App via Wi-Fi to ESP32-Mini.
//    2. At medication time:
//       - ESP32 signals Arduino: "ALERT:Drawer 1 (A1)".
//       - Buzzer Relay begins cabinet beeping.
//       - Solenoid activates to unlock drawer.
//       - LCD begins live 20-minute countdown (e.g. "19m 58s left").
//    3. User takes medicine within 20 minutes (Magnetic Reed Switch opens):
//       - Arduino detects drawer opening.
//       - Buzzer immediately silenced.
//       - Solenoid locks.
//       - LCD displays "DOSE TAKEN! :)".
//       - Arduino sends "TAKEN:Drawer 1" to ESP32-Mini -> Mobile App marks TAKEN.
//    4. User fails to take medicine within 20 minutes (Timeout):
//       - Buzzer stops & solenoid locks.
//       - LCD displays "DOSE MISSED :(".
//       - Arduino sends "MISSED:Drawer 1" to ESP32-Mini -> Mobile App marks MISSED.
// ==============================================================================

#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// ── Optional Wi-Fi for Arduino UNO R4 WiFi ───────────────────────────────────
#if defined(ARDUINO_UNOR4_WIFI) || defined(ARDUINO_ARCH_RENESAS_UNO)
  #include <WiFiS3.h>
  #define HAS_WIFI 1
#else
  #define HAS_WIFI 0
#endif

#if HAS_WIFI
  const char* ssid = "Hi";
  const char* pass = "estogs000";
  WiFiServer server(80);

  const char* wifiStatusToString(int status) {
    switch (status) {
      case WL_IDLE_STATUS: return "IDLE";
      case WL_NO_SSID_AVAIL: return "SSID NOT FOUND (Check 2.4GHz / AP Band)";
      case WL_SCAN_COMPLETED: return "SCAN COMPLETED";
      case WL_CONNECTED: return "CONNECTED";
      case WL_CONNECT_FAILED: return "AUTH / PASSWORD FAILED (Check WPA2 vs WPA3)";
      case WL_CONNECTION_LOST: return "CONNECTION LOST";
      case WL_DISCONNECTED: return "DISCONNECTED / ATTEMPTING";
      default: return "SEARCHING";
    }
  }
#endif

// ── Pin Assignments ──────────────────────────────────────────────────────────
const int REED_SWITCH_DRAWER1 = 2; // GPIO 2: Drawer 1 Magnetic Reed Switch
const int REED_SWITCH_DRAWER2 = 3; // GPIO 3: Drawer 2 Magnetic Reed Switch
const int BUZZER_RELAY_PIN    = 7; // GPIO 7: 1 Relay for Cabinet Buzzer Beep
const int SOLENOID_PIN        = 8; // GPIO 8: 1 GPIO for Solenoid Lock / Release
const int STATUS_LED_PIN      = 13;// Built-in LED

// Active Levels (Relays are typically Active-LOW, Solenoid active-HIGH via MOSFET/Relay)
const int RELAY_ON   = LOW;
const int RELAY_OFF  = HIGH;
const int SOLENOID_ON  = HIGH; // Energize to unlock
const int SOLENOID_OFF = LOW;  // De-energize to lock

// ── 20-Minute Countdown Configuration ────────────────────────────────────────
const unsigned long GRACE_PERIOD_MS = 20UL * 60UL * 1000UL; // 20 mins = 1,200,000 ms

// Initialize 16x2 I2C LCD (address 0x27 or 0x3F)
LiquidCrystal_I2C lcd(0x27, 16, 2);

// ── State Variables ──────────────────────────────────────────────────────────
bool isAlertActive = false;
unsigned long alertStartTime = 0;
String activeCompartment = "Drawer 1 (A1)";
// ── Schedule State on Arduino (For Direct Wi-Fi Operation) ───────────────────
struct ArduinoSchedule {
  int hour;          // 0 - 23 (24h)
  int minute;        // 0 - 59
  String compartment;
  int drawer;
  bool alertedToday;
  bool active;
};
const int MAX_ARDUINO_SCHEDULES = 10;
ArduinoSchedule schedules[MAX_ARDUINO_SCHEDULES];
int scheduleCount = 0;

int targetDrawer = 1; // 1 or 2

// Phone Epoch Synchronization
unsigned long clientBaseEpoch = 0;   // Epoch in local seconds from phone
unsigned long clientBaseMillis = 0;  // millis() when base epoch was received

// Buzzer cadence (BEEP: 5 seconds ON / 5 seconds OFF)
bool buzzerPhase = false;
unsigned long lastBuzzerToggle = 0;
const unsigned long BUZZER_TOGGLE_MS = 5000; // 5 seconds delay so buzzer sounds clearly

// Magnetic Reed Switch Debouncing
int lastReed1State = HIGH;
int lastReed2State = HIGH;
unsigned long lastDebounceTime1 = 0;
unsigned long lastDebounceTime2 = 0;
const unsigned long DEBOUNCE_DELAY_MS = 80;

// LCD Idle Reset Timer
unsigned long lcdResetTime = 0;
bool lcdNeedsReset = false;
unsigned long lastCountdownUpdate = 0;

// ── Helper: Update LCD Screen ────────────────────────────────────────────────
void updateDisplay(String line1, String line2) {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(line1.substring(0, 16));
  lcd.setCursor(0, 1);
  lcd.print(line2.substring(0, 16));
}

void scheduleIdleScreen(unsigned long delayMs = 3000) {
  lcdResetTime = millis() + delayMs;
  lcdNeedsReset = true;
}

// ── Start 20-Minute Alert ────────────────────────────────────────────────────
void startAlert(const String& compartment, int drawerNum = 1) {
  isAlertActive = true;
  alertStartTime = millis();
  activeCompartment = compartment;
  targetDrawer = drawerNum;

  // 1. Unlock Solenoid so patient can pull drawer
  digitalWrite(SOLENOID_PIN, SOLENOID_ON);

  // 2. Start Buzzer Relay
  buzzerPhase = true;
  digitalWrite(BUZZER_RELAY_PIN, RELAY_ON);
  lastBuzzerToggle = millis();

  // 3. Display Alert & 20-minute countdown on LCD
  updateDisplay("DUE: " + compartment, "20m 00s left");
  lastCountdownUpdate = millis();

  Serial.print("[ARDUINO] 🚨 20-Minute Alert Started for: ");
  Serial.println(compartment);
}

// ── Resolve Dose: TAKEN (Magnetic Reed Switch Triggered) ──────────────────────
void resolveDoseTaken(int drawerNum) {
  if (!isAlertActive) return;

  unsigned long elapsedMs = millis() - alertStartTime;
  int elapsedSec = elapsedMs / 1000;

  isAlertActive = false;

  // 1. Silence Buzzer Relay
  digitalWrite(BUZZER_RELAY_PIN, RELAY_OFF);

  // 2. Relock Solenoid after 3 seconds
  digitalWrite(SOLENOID_PIN, SOLENOID_OFF);

  // 3. Update LCD Screen
  updateDisplay("DOSE TAKEN! :)", "Drawer " + String(drawerNum) + " Opened");
  scheduleIdleScreen(4000);

  // 4. Send Confirmation over Serial to ESP32-Mini (which updates Mobile App)
  Serial.print("TAKEN:");
  Serial.println(activeCompartment);

  Serial.print("[ARDUINO] ✅ Medication retrieved from Drawer ");
  Serial.print(drawerNum);
  Serial.print(" in ");
  Serial.print(elapsedSec);
  Serial.println(" seconds.");
}

// ── Resolve Dose: MISSED (20-Minute Timeout Exceeded) ────────────────────────
void resolveDoseMissed() {
  isAlertActive = false;

  // 1. Turn off Buzzer Relay & Lock Solenoid
  digitalWrite(BUZZER_RELAY_PIN, RELAY_OFF);
  digitalWrite(SOLENOID_PIN, SOLENOID_OFF);

  // 2. Update LCD Screen
  updateDisplay("DOSE MISSED :(", activeCompartment);
  scheduleIdleScreen(5000);

  // 3. Send Missed Event over Serial to ESP32-Mini
  Serial.print("MISSED:");
  Serial.println(activeCompartment);

  Serial.println("[ARDUINO] ⌛ 20-Minute Timeout Exceeded: Marked MISSED.");
}

// ── Manual Dispense & Relay Pulse (5 Seconds) ──────────────────────────────
void triggerDispenseSolenoid(unsigned long durationMs = 5000) {
  Serial.println("[ARDUINO] Relay Dispense & Buzzer Pulse Triggered (5 seconds)");
  updateDisplay("DISPENSING...", activeCompartment);

  // Activate Buzzer Relay & Solenoid for 5 seconds
  digitalWrite(BUZZER_RELAY_PIN, RELAY_ON);
  digitalWrite(SOLENOID_PIN, SOLENOID_ON);
  delay(durationMs);
  digitalWrite(BUZZER_RELAY_PIN, RELAY_OFF);
  digitalWrite(SOLENOID_PIN, SOLENOID_OFF);

  updateDisplay("DISPENSED!", "Drawer Unlocked");
  scheduleIdleScreen(2500);
}

// ── Standalone Buzzer Relay Sound (5 Seconds) ────────────────────────────────
void triggerBuzzerRelay(unsigned long durationMs = 5000) {
  Serial.println("[ARDUINO] Buzzer Relay Sounding for 5 seconds");
  updateDisplay("BUZZER ACTIVE", "Sounding 5s...");

  digitalWrite(BUZZER_RELAY_PIN, RELAY_ON);
  delay(durationMs);
  digitalWrite(BUZZER_RELAY_PIN, RELAY_OFF);

  updateDisplay("Smart Dispenser", "Ready");
  scheduleIdleScreen(2000);
}

// ── Helper: Extract HTTP Query Parameter ─────────────────────────────────────
String getQueryParam(const String& req, const String& param) {
  int pos = req.indexOf(param + "=");
  if (pos == -1) return "";
  int start = pos + param.length() + 1;
  int end = req.indexOf('&', start);
  if (end == -1) end = req.indexOf(' ', start);
  if (end == -1) end = req.indexOf('\r', start);
  if (end == -1) end = req.length();
  return req.substring(start, end);
}

// ── Check Scheduled Alarms on Arduino ────────────────────────────────────────
void checkArduinoSchedules() {
  if (clientBaseEpoch == 0 || isAlertActive || scheduleCount == 0) return;

  unsigned long curEpoch = clientBaseEpoch + ((millis() - clientBaseMillis) / 1000);
  // Add 8 hours for Philippines / Manila timezone (GMT+8)
  unsigned long localEpoch = curEpoch + 28800UL;
  int curHour = (localEpoch % 86400) / 3600;
  int curMin  = (localEpoch % 3600) / 60;

  for (int i = 0; i < scheduleCount; i++) {
    if (schedules[i].active && schedules[i].hour == curHour && schedules[i].minute == curMin) {
      if (!schedules[i].alertedToday) {
        schedules[i].alertedToday = true;
        Serial.print("[ARDUINO] 🚨 SCHEDULE DUE! Ringing Buzzer for: ");
        Serial.println(schedules[i].compartment);
        startAlert(schedules[i].compartment, schedules[i].drawer);
      }
    } else if (schedules[i].minute != curMin) {
      schedules[i].alertedToday = false;
    }
  }
}

// ── Monitor Magnetic Reed Switches ───────────────────────────────────────────
void checkMagneticReedSwitches() {
  int r1 = digitalRead(REED_SWITCH_DRAWER1);
  int r2 = digitalRead(REED_SWITCH_DRAWER2);

  unsigned long now = millis();

  // Reed Switch 1 (Drawer 1)
  // With INPUT_PULLUP: Closed = LOW (magnet near), Open = HIGH (drawer pulled open)
  if (r1 != lastReed1State) {
    lastDebounceTime1 = now;
  }
  if ((now - lastDebounceTime1) > DEBOUNCE_DELAY_MS) {
    if (r1 == HIGH && isAlertActive && targetDrawer == 1) {
      resolveDoseTaken(1);
    }
  }
  lastReed1State = r1;

  // Reed Switch 2 (Drawer 2)
  if (r2 != lastReed2State) {
    lastDebounceTime2 = now;
  }
  if ((now - lastDebounceTime2) > DEBOUNCE_DELAY_MS) {
    if (r2 == HIGH && isAlertActive && targetDrawer == 2) {
      resolveDoseTaken(2);
    }
  }
  lastReed2State = r2;
}

// ── Process Serial Commands from ESP32-Mini ──────────────────────────────────
void processSerialCommand(const String& cmd) {
  if (cmd.startsWith("ALERT:")) {
    // E.g. "ALERT:Drawer 1 (A1)" or "ALERT:Drawer 2 (B3)"
    String comp = cmd.substring(6);
    int drawer = 1;
    if (comp.indexOf("Drawer 2") != -1 || comp.indexOf("2") == 0) {
      drawer = 2;
    }
    startAlert(comp, drawer);
  }
  else if (cmd.startsWith("DISPENSE")) {
    triggerDispenseSolenoid(5000);
  }
  else if (cmd.startsWith("BUZZER") || cmd.startsWith("RELAY") || cmd.startsWith("BEEP")) {
    triggerBuzzerRelay(5000);
  }
  else if (cmd.startsWith("TAKEN:")) {
    // Manually confirmed from mobile app
    String comp = cmd.substring(6);
    isAlertActive = false;
    digitalWrite(BUZZER_RELAY_PIN, RELAY_OFF);
    digitalWrite(SOLENOID_PIN, SOLENOID_OFF);
    updateDisplay("DOSE TAKEN! :)", comp);
    scheduleIdleScreen(3000);
  }
  else if (cmd.startsWith("MISSED:")) {
    String comp = cmd.substring(7);
    resolveDoseMissed();
  }
  else if (cmd == "STOP" || cmd == "SILENCE") {
    isAlertActive = false;
    digitalWrite(BUZZER_RELAY_PIN, RELAY_OFF);
    digitalWrite(SOLENOID_PIN, SOLENOID_OFF);
    updateDisplay("Alert Stopped", "Smart Dispenser");
    scheduleIdleScreen(2000);
  }
  else if (cmd.startsWith("IP:")) {
    String ipStr = cmd.substring(3);
    updateDisplay("WiFi Connected!", ipStr);
    scheduleIdleScreen(6000);
  }
  else if (cmd == "PING") {
    Serial.println("PONG:OK");
    updateDisplay("App Connected", "Ready");
    scheduleIdleScreen(1500);
  }
}

// ── Setup ────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);

  // 1. Initialize I2C LCD Display
  lcd.init();
  lcd.backlight();
  updateDisplay("Smart Dispenser", "Initializing...");

  // 2. Initialize Buzzer Relay Pin (1 GPIO)
  pinMode(BUZZER_RELAY_PIN, OUTPUT);
  digitalWrite(BUZZER_RELAY_PIN, RELAY_OFF);

  // 3. Initialize Solenoid Lock Pin (1 GPIO)
  pinMode(SOLENOID_PIN, OUTPUT);
  digitalWrite(SOLENOID_PIN, SOLENOID_OFF);

  // 4. Initialize Magnetic Reed Switches (2 GPIOs with internal pull-up)
  pinMode(REED_SWITCH_DRAWER1, INPUT_PULLUP);
  pinMode(REED_SWITCH_DRAWER2, INPUT_PULLUP);

  pinMode(STATUS_LED_PIN, OUTPUT);
  digitalWrite(STATUS_LED_PIN, LOW);

  delay(1200);

#if HAS_WIFI
  Serial.print("[ARDUINO] Initializing WiFi for SSID '");
  Serial.print(ssid);
  Serial.println("'...");
  updateDisplay("Connecting WiFi", String(ssid).substring(0, 16));

  WiFi.disconnect();
  delay(200);

  int attempt = 0;
  while (WiFi.status() != WL_CONNECTED && attempt < 5) {
    attempt++;
    Serial.print("[ARDUINO] Connect attempt ");
    Serial.print(attempt);
    Serial.print("/5... ");
    WiFi.begin(ssid, pass);

    int waitDots = 0;
    while (WiFi.status() != WL_CONNECTED && waitDots < 14) {
      delay(500);
      Serial.print(".");
      waitDots++;
    }
    Serial.print(" Result: ");
    Serial.println(wifiStatusToString(WiFi.status()));
    if (WiFi.status() == WL_CONNECTED) break;
    delay(500);
  }

  if (WiFi.status() == WL_CONNECTED) {
    server.begin();
    String ipStr = WiFi.localIP().toString();
    updateDisplay("WiFi Connected!", ipStr);
    Serial.print("[ARDUINO] ✅ WiFi Connected! IP Address: ");
    Serial.println(ipStr);
    delay(2000);
  } else {
    Serial.print("[ARDUINO] ⚠️ Final WiFi Status: ");
    Serial.println(wifiStatusToString(WiFi.status()));
    Serial.println("[ARDUINO] If 'SSID NOT FOUND': Verify phone hotspot has 2.4GHz / Maximize Compatibility ON.");
    Serial.println("[ARDUINO] If 'AUTH FAILED': Verify router/hotspot uses WPA2-Personal (not WPA3).");
    updateDisplay("WiFi Offline", "Using Serial");
    delay(1500);
  }
#else
  Serial.println("[ARDUINO] Running in Standard Serial-Slave mode (No Onboard WiFi chip).");
  Serial.println("[ARDUINO] Controlled by ESP32-Mini Master via UART Pin 0 (RX) & Pin 1 (TX).");
#endif

  updateDisplay("Smart Dispenser", "Waiting Schedule");
  Serial.println("[ARDUINO] Ready! Listening on Serial & Sensors.");
}

// ── Loop ─────────────────────────────────────────────────────────────────────
void loop() {
  // 1. Check Serial Commands from ESP32-Mini Master
  if (Serial.available() > 0) {
    String command = Serial.readStringUntil('\n');
    command.trim();
    if (command.length() > 0) {
      processSerialCommand(command);
    }
  }

  // 2. Check Magnetic Reed Switches (Detects physical drawer opening)
  checkMagneticReedSwitches();

  // 3. Active 20-Minute Alert State Machine
  if (isAlertActive) {
    unsigned long elapsed = millis() - alertStartTime;

    // Check 20-minute grace period expiry (Timeout)
    if (elapsed >= GRACE_PERIOD_MS) {
      resolveDoseMissed();
    } else {
      // Toggle Buzzer Relay in cadence (Beep-Beep)
      if (millis() - lastBuzzerToggle >= BUZZER_TOGGLE_MS) {
        lastBuzzerToggle = millis();
        buzzerPhase = !buzzerPhase;
        digitalWrite(BUZZER_RELAY_PIN, buzzerPhase ? RELAY_ON : RELAY_OFF);
      }

      // Update countdown display on LCD once per second
      if (millis() - lastCountdownUpdate >= 1000) {
        lastCountdownUpdate = millis();
        unsigned long remainingMs = GRACE_PERIOD_MS - elapsed;
        int remSecTotal = remainingMs / 1000;
        int remMin = remSecTotal / 60;
        int remSec = remSecTotal % 60;

        char timeBuf[17];
        snprintf(timeBuf, sizeof(timeBuf), "%02dm %02ds left", remMin, remSec);
        lcd.setCursor(0, 1);
        lcd.print(timeBuf);
      }
    }
  }

  // 4. LCD Auto-Reset back to idle when scheduled
  if (!isAlertActive && lcdNeedsReset && millis() >= lcdResetTime) {
    lcdNeedsReset = false;
    updateDisplay("Smart Dispenser", "Waiting Schedule");
  }

  // 5. Check Scheduled Alarms on Arduino
  checkArduinoSchedules();

#if HAS_WIFI
  // 6. Direct Wi-Fi Web Server for Uno R4 WiFi
  WiFiClient client = server.available();
  if (client) {
    String req = client.readStringUntil('\r');
    client.flush();

    if (req.indexOf("/set-schedule") != -1) {
      int h = getQueryParam(req, "hour").toInt();
      int m = getQueryParam(req, "minute").toInt();
      String amPm = getQueryParam(req, "amPm");
      String comp = getQueryParam(req, "compartment");
      comp.replace('+', ' ');
      String epochStr = getQueryParam(req, "client_epoch");
      int drw = (comp.indexOf("Drawer 2") != -1) ? 2 : 1;

      // Convert 12h to 24h
      int h24 = h;
      if (amPm.equalsIgnoreCase("AM") && h == 12) h24 = 0;
      else if (amPm.equalsIgnoreCase("PM") && h != 12) h24 = h + 12;

      // Calibrate local clock from phone client_epoch
      if (epochStr.length() > 0) {
        clientBaseEpoch = (unsigned long)epochStr.toInt();
        clientBaseMillis = millis();
      }

      // Add or update schedule
      bool updated = false;
      for (int i = 0; i < scheduleCount; i++) {
        if (schedules[i].compartment == comp) {
          schedules[i].hour = h24;
          schedules[i].minute = m;
          schedules[i].drawer = drw;
          schedules[i].alertedToday = false;
          schedules[i].active = true;
          updated = true;
          break;
        }
      }
      if (!updated && scheduleCount < MAX_ARDUINO_SCHEDULES) {
        schedules[scheduleCount].hour = h24;
        schedules[scheduleCount].minute = m;
        schedules[scheduleCount].compartment = comp.length() > 0 ? comp : "Drawer 1 (A1)";
        schedules[scheduleCount].drawer = drw;
        schedules[scheduleCount].alertedToday = false;
        schedules[scheduleCount].active = true;
        scheduleCount++;
      }

      char dispTime[17];
      snprintf(dispTime, sizeof(dispTime), "%02d:%02d %s", h, m, amPm.c_str());
      updateDisplay("Schedule Saved!", String(dispTime));
      scheduleIdleScreen(3000);

      Serial.print("[ARDUINO] 📅 Schedule Synced: ");
      Serial.print(dispTime);
      Serial.print(" (24h: ");
      Serial.print(h24);
      Serial.print(":");
      Serial.print(m);
      Serial.print(") for ");
      Serial.println(comp);

      client.println("HTTP/1.1 200 OK");
      client.println("Content-Type: application/json");
      client.println("Connection: close");
      client.println("Access-Control-Allow-Origin: *");
      client.println();
      client.println("{\"status\":\"ok\"}");
      delay(15);
      client.stop();
    }
    else if (req.indexOf("/alert") != -1) {
      String comp = getQueryParam(req, "compartment");
      comp.replace('+', ' ');
      if (comp.length() == 0) comp = "Drawer 1 (A1)";
      int drw = (comp.indexOf("Drawer 2") != -1) ? 2 : 1;
      client.println("HTTP/1.1 200 OK");
      client.println("Content-Type: application/json");
      client.println("Connection: close");
      client.println("Access-Control-Allow-Origin: *");
      client.println();
      client.println("{\"status\":\"ok\",\"action\":\"alert_started\"}");
      delay(15);
      client.stop();
      startAlert(comp, drw);
    }
    else if (req.indexOf("/dispense") != -1) {
      client.println("HTTP/1.1 200 OK");
      client.println("Content-Type: application/json");
      client.println("Connection: close");
      client.println("Access-Control-Allow-Origin: *");
      client.println();
      client.println("{\"status\":\"ok\"}");
      delay(15);
      client.stop();
      triggerDispenseSolenoid(5000);
    } else if (req.indexOf("/buzzer") != -1 || req.indexOf("/relay") != -1) {
      client.println("HTTP/1.1 200 OK");
      client.println("Content-Type: application/json");
      client.println("Connection: close");
      client.println("Access-Control-Allow-Origin: *");
      client.println();
      client.println("{\"status\":\"ok\",\"action\":\"buzzer_5s\"}");
      delay(15);
      client.stop();
      triggerBuzzerRelay(5000);
    } else if (req.indexOf("/stop-vibrate") != -1 || req.indexOf("/confirm") != -1) {
      client.println("HTTP/1.1 200 OK");
      client.println("Content-Type: application/json");
      client.println("Connection: close");
      client.println("Access-Control-Allow-Origin: *");
      client.println();
      client.println("{\"status\":\"ok\",\"action\":\"stopped\"}");
      delay(15);
      client.stop();
      resolveDoseTaken(1);
    } else if (req.indexOf("/status") != -1 || req.indexOf("/ping") != -1) {
      String json = "{\"status\":\"online\",\"is_alerting\":" + String(isAlertActive ? "true" : "false") + ",\"schedules\":" + String(scheduleCount) + "}";
      client.println("HTTP/1.1 200 OK");
      client.println("Content-Type: application/json");
      client.println("Connection: close");
      client.println("Access-Control-Allow-Origin: *");
      client.println();
      client.println(json);
      delay(15);
      client.stop();
    } else if (req.startsWith("OPTIONS")) {
      client.println("HTTP/1.1 204 No Content");
      client.println("Access-Control-Allow-Origin: *");
      client.println("Access-Control-Allow-Methods: GET, POST, OPTIONS");
      client.println("Access-Control-Allow-Headers: *");
      client.println("Connection: close");
      client.println();
      delay(15);
      client.stop();
    } else {
      client.println("HTTP/1.1 404 Not Found");
      client.println("Connection: close");
      client.println();
      client.println("Not Found");
      delay(15);
      client.stop();
    }
  }
#endif
}