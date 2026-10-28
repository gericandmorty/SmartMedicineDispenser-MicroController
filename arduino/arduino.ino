// ==============================================================================
//  SMART MEDICINE DISPENSER - ARDUINO CONTROLLER (UNO R3 / UNO R4 WiFi)
//  Firmware Version: 3.3 (Dual Solenoid Locks for Drawer 1 & Drawer 2 via Relays)
// ==============================================================================
//  Hardware Setup:
//    1. I2C LCD 16x2 Display:
//       - SDA -> Arduino SDA (Pin A4 on Uno R3)
//       - SCL -> Arduino SCL (Pin A5 on Uno R3)
//       - Auto-detects I2C Address: 0x27 or 0x3F
//
//    2. Buzzer Relay Module (1 GPIO):
//       - Pin 7 -> Relay IN (1s BEEP / 1s PAUSE, 2-Minute Maximum Duration)
//
//    3. Dual Solenoid Locks (Connected to Relay Module - Active-LOW):
//       - Pin 8 -> Relay IN (Controls Solenoid Lock for Drawer 1)
//       - Pin 9 -> Relay IN (Controls Solenoid Lock for Drawer 2)
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
//  Logic Features:
//    1. Dual Independent Solenoids via Relays:
//       - When Drawer 1 is due: Pin 8 Relay activates to unlock Drawer 1 solenoid.
//       - When Drawer 2 is due: Pin 9 Relay activates to unlock Drawer 2 solenoid.
//       - When Drawer 1 is pulled open: Pin 8 Relay locks after 3s.
//       - When Drawer 2 is pulled open: Pin 9 Relay locks after 3s.
//    2. 2-Minute Buzzer Cutoff:
//       - Buzzer beeps (1s ON / 1s OFF) for 2 minutes max, then silences.
//       - 20-minute alert grace period & live countdown on LCD continues.
//    3. Magnetic Reed Switch & LED Behavior:
//       - Drawer 1 Reed Switch (Pin 2) controls ESP1 LEDs (Drawer 1 Matrix).
//       - Drawer 2 Reed Switch (Pin 3) controls ESP2 LEDs (Drawer 2 Matrix).
//       - When reed switch is CLOSED: LEDs on ESP1 / ESP2 will NOT lit up (allOff).
//       - When reed switch is OPEN: LEDs on ESP1 / ESP2 will lit up (only for scheduled/alerting cells).
//       - When drawer is pulled open during an alert: Buzzer silences, dose is taken on drawer close, and LEDs stay illuminated while open.
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
  #include <WiFiUdp.h>
  const char* ssid = "Hi";            // Matching ESP1, ESP2, & ESP32-Mini
  const char* pass = "estogs000";
  WiFiServer server(80);
  WiFiUDP udpDiscovery;
  const unsigned int UDP_DISCOVERY_PORT = 4210;
  IPAddress esp1IP = INADDR_NONE;
  IPAddress esp2IP = INADDR_NONE;
  IPAddress esp32IP = INADDR_NONE;
  unsigned long lastDiscoveryBeacon = 0;

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
const int REED_SWITCH_DRAWER1   = 2; // GPIO 2: Drawer 1 Magnetic Reed Switch
const int REED_SWITCH_DRAWER2   = 3; // GPIO 3: Drawer 2 Magnetic Reed Switch
const int BUZZER_RELAY_PIN      = 7; // GPIO 7: Cabinet Buzzer Relay (Active-LOW)
const int SOLENOID_DRAWER1_PIN  = 8; // GPIO 8: Solenoid Relay for Drawer 1 (Active-LOW)
const int SOLENOID_DRAWER2_PIN  = 9; // GPIO 9: Solenoid Relay for Drawer 2 (Active-LOW)
const int STATUS_LED_PIN        = 13; // Built-in / Cabinet Indicator LED (Lit ONLY when reed switch is OPEN)

// ── Active Levels for Relay Modules (Standard Relay Modules are Active-LOW) ──
const int RELAY_ON      = LOW;  // Active-LOW relay ON (Energized)
const int RELAY_OFF     = HIGH; // Active-LOW relay OFF (De-energized)
const int SOLENOID_ON   = LOW;  // Relay LOW -> Energizes Solenoid (Unlocks Door)
const int SOLENOID_OFF  = HIGH; // Relay HIGH -> De-energizes Solenoid (Locks Door)

// ── Timing Configurations ────────────────────────────────────────────────────
const unsigned long GRACE_PERIOD_MS    = 20UL * 60UL * 1000UL; // 20 mins = 1,200,000 ms
const unsigned long BUZZER_DURATION_MS = 2UL * 60UL * 1000UL;  // 2 minutes buzzing cutoff = 120,000 ms

// Initialize 16x2 I2C LCD (Auto-detected in setup to 0x27 or 0x3F)
LiquidCrystal_I2C lcd(0x27, 16, 2);

// ── Multi-Compartment Alert Data Structures ──────────────────────────────────
const int MAX_ACTIVE_ALERTS = 8;
struct AlertCompartment {
  String shortLabel; // e.g. "B1", "B2", "B3"
  String fullName;   // e.g. "Drawer 1 (B1)"
  int drawer;        // 1 or 2
  bool taken;
};

AlertCompartment activeAlerts[MAX_ACTIVE_ALERTS];
int alertCount = 0;

// LCD Cycling / Incrementing State
int currentDisplayIdx = 0;
unsigned long lastDisplayCycleMs = 0;
const unsigned long DISPLAY_CYCLE_INTERVAL_MS = 2000; // Increment every 2 seconds

// ── Alert State Variables ────────────────────────────────────────────────────
bool isAlertActive = false;
unsigned long alertStartTime = 0;

// Buzzer Cadence (1s BEEP / 1s PAUSE during the 2-minute buzzing window)
bool buzzerState = false;
unsigned long lastBuzzerToggle = 0;
const unsigned long BUZZER_BEEP_MS  = 1000; // 1s BEEP
const unsigned long BUZZER_PAUSE_MS = 1000; // 1s PAUSE

// Non-blocking Solenoid Relock Timers (Independent per drawer)
unsigned long solenoid1OffTime = 0;
bool solenoid1PulseActive = false;

unsigned long solenoid2OffTime = 0;
bool solenoid2PulseActive = false;

// Non-blocking Buzzer pulse timer (Manual tests)
unsigned long buzzerPulseOffTime = 0;
bool buzzerPulseActive = false;

// Fixed reed polarity: LOW = CLOSED (magnet near), HIGH = OPEN (magnet away)
int lastReed1State = HIGH;
int lastReed2State = HIGH;
bool drawer1IsOpen = false;
bool drawer2IsOpen = false;
unsigned long lastDebounceTime1 = 0;
unsigned long lastDebounceTime2 = 0;
const unsigned long DEBOUNCE_DELAY_MS = 60;

// Dose Taken Reporting for Mobile App Status
bool lastDoseTaken = false;
unsigned long lastDoseTakenMillis = 0;
int lastDoseTakenDrawer = 0;
String lastTakenSummary = "";

// LCD Display & Idle State
unsigned long lcdResetTime = 0;
bool lcdNeedsReset = false;
unsigned long lastCountdownUpdate = 0;
String lastLine1 = "";
String lastLine2 = "";
String deviceIPStr = "";

// Schedules for Direct Wi-Fi Operation (Uno R4 WiFi)
struct ArduinoSchedule {
  int hour;
  int minute;
  String compartment;
  int drawer;
  bool alertedToday;
  bool active;
};
const int MAX_ARDUINO_SCHEDULES = 10;
ArduinoSchedule schedules[MAX_ARDUINO_SCHEDULES];
int scheduleCount = 0;

// Phone Epoch Synchronization
unsigned long clientBaseEpoch = 0;
unsigned long clientBaseMillis = 0;

// ── Helper: Extract Short Label (e.g. "Drawer 1 (B1)" -> "B1") ──────────────
String extractShortLabel(const String& str) {
  String s = str;
  s.trim();
  s.toUpperCase();

  int p1 = s.indexOf('(');
  if (p1 != -1) {
    int p2 = s.indexOf(')', p1);
    if (p2 != -1) {
      s = s.substring(p1 + 1, p2);
      s.trim();
    }
  }

  for (int i = 0; i < s.length(); i++) {
    char c = s.charAt(i);
    if (c >= 'A' && c <= 'G') {
      if (i + 1 < s.length() && s.charAt(i + 1) >= '1' && s.charAt(i + 1) <= '3') {
        return s.substring(i, i + 2);
      }
    }
  }

  return s.length() > 6 ? s.substring(0, 6) : s;
}

// ── Helper: Format and write a fixed 16-character line to LCD without lcd.clear() ──
void writeLCDLine(uint8_t row, const String& text) {
  char buf[17];
  int len = text.length();
  for (int i = 0; i < 16; i++) {
    buf[i] = (i < len) ? text[i] : ' ';
  }
  buf[16] = '\0';
  lcd.setCursor(0, row);
  lcd.print(buf);
}

// ── Helper: Update LCD Screen (Avoid redundant redraws & eliminates lcd.clear() flicker/corruption) ──
void updateDisplay(const String& line1, const String& line2) {
  if (line1 == lastLine1 && line2 == lastLine2) return;
  lastLine1 = line1;
  lastLine2 = line2;

  writeLCDLine(0, line1);
  writeLCDLine(1, line2);
}

void scheduleIdleScreen(unsigned long delayMs = 3500) {
  if (isAlertActive) return; // Never schedule idle screen during alert
  lcdResetTime = millis() + delayMs;
  lcdNeedsReset = true;
}

void showIdleScreen() {
  if (isAlertActive) return; // Never overwrite an active alert
  if (deviceIPStr.length() > 0) {
    updateDisplay("Smart Dispenser", "IP: " + deviceIPStr);
  } else {
    updateDisplay("Smart Dispenser", "Ready & Watching");
  }
}

// ── Helper: URL Decoder (%20, +, %28, %29, etc.) ────────────────────────────
String urlDecode(const String& input) {
  String decoded = "";
  for (int i = 0; i < input.length(); i++) {
    if (input[i] == '%') {
      if (i + 2 < input.length()) {
        char a = input[i + 1];
        char b = input[i + 2];
        if (isxdigit(a) && isxdigit(b)) {
          int valA = (a >= 'a') ? (a - 'a' + 10) : ((a >= 'A') ? (a - 'A' + 10) : (a - '0'));
          int valB = (b >= 'a') ? (b - 'a' + 10) : ((b >= 'A') ? (b - 'A' + 10) : (b - '0'));
          decoded += (char)(16 * valA + valB);
          i += 2;
          continue;
        }
      }
    } else if (input[i] == '+') {
      decoded += ' ';
    } else {
      decoded += input[i];
    }
  }
  return decoded;
}

// ── Notify ESP32 Master to Start/Stop Vibration over Serial & Wi-Fi ──────────
void notifyESP32Vibrate(bool start = true) {
  // 1. Send UART command to ESP32-Mini (Works over wired bridge)
  Serial.println(start ? "VIBRATE" : "STOP");

#if HAS_WIFI
  // 2. Also send over Wi-Fi if ESP32 IP was auto-discovered
  if (WiFi.status() == WL_CONNECTED && esp32IP != INADDR_NONE) {
    WiFiClient cli;
    if (cli.connect(esp32IP, 80)) {
      String endpoint = start ? "/vibrate?duration=60000&pulses=1" : "/stop-vibrate";
      cli.print(String("GET ") + endpoint + " HTTP/1.1\r\n" +
                "Host: " + esp32IP.toString() + "\r\n" +
                "Connection: close\r\n\r\n");
      delay(15);
      cli.stop();
    }
  }
#endif
}

// ── Solenoid Relay Controllers (Drawer 1 on Pin 8, Drawer 2 on Pin 9) ───────
void unlockDrawer(int drawerNum) {
  if (drawerNum == 1 || drawerNum == 0) {
    digitalWrite(SOLENOID_DRAWER1_PIN, SOLENOID_ON);
    solenoid1PulseActive = false;
    Serial.println("[ARDUINO] Solenoid Relay 1 (Drawer 1 / Pin 8) UNLOCKED.");
  }
  if (drawerNum == 2 || drawerNum == 0) {
    digitalWrite(SOLENOID_DRAWER2_PIN, SOLENOID_ON);
    solenoid2PulseActive = false;
    Serial.println("[ARDUINO] Solenoid Relay 2 (Drawer 2 / Pin 9) UNLOCKED.");
  }
}

void lockDrawer(int drawerNum) {
  if (drawerNum == 1 || drawerNum == 0) {
    digitalWrite(SOLENOID_DRAWER1_PIN, SOLENOID_OFF);
    solenoid1PulseActive = false;
    Serial.println("[ARDUINO] Solenoid Relay 1 (Drawer 1 / Pin 8) LOCKED.");
  }
  if (drawerNum == 2 || drawerNum == 0) {
    digitalWrite(SOLENOID_DRAWER2_PIN, SOLENOID_OFF);
    solenoid2PulseActive = false;
    Serial.println("[ARDUINO] Solenoid Relay 2 (Drawer 2 / Pin 9) LOCKED.");
  }
}

// ── Add Compartment to Active Alert List ──────────────────────────────────────
void startAlert(const String& rawComp, int drawerNum = 1) {
  String remaining = rawComp;
  remaining.trim();

  // Split multiple compartments if separated by commas or semicolons
  while (remaining.length() > 0) {
    int sepIdx = remaining.indexOf(',');
    if (sepIdx == -1) sepIdx = remaining.indexOf(';');
    String token = (sepIdx != -1) ? remaining.substring(0, sepIdx) : remaining;
    if (sepIdx != -1) remaining = remaining.substring(sepIdx + 1);
    else remaining = "";
    token.trim();
    if (token.length() == 0) continue;

    int drw = drawerNum;
    String upperToken = token;
    upperToken.toUpperCase();
    if (upperToken.indexOf("DRAWER 2") != -1 || upperToken.indexOf("2:") == 0 || upperToken.indexOf("(2") != -1) drw = 2;
    else if (upperToken.indexOf("DRAWER 1") != -1 || upperToken.indexOf("1:") == 0 || upperToken.indexOf("(1") != -1) drw = 1;

    String shortLabel = extractShortLabel(token);

    // Prevent duplicate entries for active untaken alert
    bool alreadyExists = false;
    for (int i = 0; i < alertCount; i++) {
      if (activeAlerts[i].shortLabel == shortLabel && activeAlerts[i].drawer == drw && !activeAlerts[i].taken) {
        alreadyExists = true;
        break;
      }
    }

    if (!alreadyExists && alertCount < MAX_ACTIVE_ALERTS) {
      activeAlerts[alertCount].shortLabel = shortLabel;
      activeAlerts[alertCount].fullName = token;
      activeAlerts[alertCount].drawer = drw;
      activeAlerts[alertCount].taken = false;
      alertCount++;

      // Unlock ONLY the corresponding drawer solenoid relay
      unlockDrawer(drw);

      Serial.print("[ARDUINO] 🚨 Alert Compartment Queued: ");
      Serial.print(shortLabel);
      Serial.print(" (Drawer ");
      Serial.print(drw);
      Serial.println(")");
    }
  }

  // Start 20-minute countdown and 2-minute buzzer if not already alerting
  if (!isAlertActive && alertCount > 0) {
    isAlertActive = true;
    lastDoseTaken = false;
    alertStartTime = millis();

    // Start Buzzer Relay (Will automatically stop after 2 minutes)
    buzzerState = true;
    digitalWrite(BUZZER_RELAY_PIN, RELAY_ON);
    lastBuzzerToggle = millis();

    currentDisplayIdx = 0;
    lastDisplayCycleMs = millis();
    lastCountdownUpdate = millis();

    // Trigger ESP32 Vibration over Serial UART & Wi-Fi
    notifyESP32Vibrate(true);

    Serial.println("[ARDUINO] 20-Minute Alert Window Activated. Buzzer active for 2 minutes.");
  }
}

// ── Resolve Doses in Drawer (Magnetic Reed Switch Triggered) ──────────────────
void resolveDoseTaken(int drawerNum) {
  if (!isAlertActive) return;

  unsigned long elapsedMs = millis() - alertStartTime;
  int elapsedSec = elapsedMs / 1000;

  String takenSummary = "";
  int takenFromThisDrawer = 0;

  // Mark all untaken doses in this opened drawer as TAKEN
  for (int i = 0; i < alertCount; i++) {
    if (!activeAlerts[i].taken && (activeAlerts[i].drawer == drawerNum || drawerNum == 0)) {
      activeAlerts[i].taken = true;
      if (takenSummary.length() > 0) takenSummary += ", ";
      takenSummary += activeAlerts[i].shortLabel;
      takenFromThisDrawer++;

      // Send TAKEN event for each compartment over UART Serial to ESP32-Mini
      Serial.print("TAKEN:");
      Serial.println(activeAlerts[i].fullName);
    }
  }

  if (takenFromThisDrawer == 0) return;

  // Relock this drawer's solenoid relay after 3s delay (so user can close drawer)
  if (drawerNum == 1 || drawerNum == 0) {
    solenoid1OffTime = millis() + 3000;
    solenoid1PulseActive = true;
  }
  if (drawerNum == 2 || drawerNum == 0) {
    solenoid2OffTime = millis() + 3000;
    solenoid2PulseActive = true;
  }

  // Check if any untaken alerts still remain in the other drawer
  int remainingAlerts = 0;
  for (int i = 0; i < alertCount; i++) {
    if (!activeAlerts[i].taken) remainingAlerts++;
  }

  if (remainingAlerts == 0) {
    // All doses resolved! Silence buzzer and stop vibration
    isAlertActive = false;
    lastDoseTaken = true;
    lastDoseTakenMillis = millis();
    lastDoseTakenDrawer = drawerNum;
    lastTakenSummary = takenSummary;
    digitalWrite(BUZZER_RELAY_PIN, RELAY_OFF);
    buzzerState = false;

    // Silence ESP32 vibration
    notifyESP32Vibrate(false);

    updateDisplay("DOSE TAKEN! :)", takenSummary.length() > 0 ? (takenSummary + " OK") : "Drawer Closed");
    scheduleIdleScreen(4000);
    alertCount = 0;

    Serial.print("[ARDUINO] ✅ All doses retrieved in ");
    Serial.print(elapsedSec);
    Serial.println("s.");
  } else {
    // Doses in other drawer still pending
    updateDisplay("DOSE TAKEN! :)", takenSummary + " OK");
    scheduleIdleScreen(2500);
    Serial.print("[ARDUINO] ✅ Drawer ");
    Serial.print(drawerNum);
    Serial.print(" taken. Still waiting on other drawer (");
    Serial.print(remainingAlerts);
    Serial.println(" left).");
  }
}

// ── Resolve Dose: MISSED (20-Minute Timeout Exceeded) ────────────────────────
void resolveDoseMissed() {
  isAlertActive = false;

  // Silence buzzer & lock both solenoid relays
  digitalWrite(BUZZER_RELAY_PIN, RELAY_OFF);
  lockDrawer(0);
  buzzerState = false;

  // Silence ESP32 vibration
  notifyESP32Vibrate(false);

  String missedList = "";
  for (int i = 0; i < alertCount; i++) {
    if (!activeAlerts[i].taken) {
      if (missedList.length() > 0) missedList += ", ";
      missedList += activeAlerts[i].shortLabel;
      Serial.print("MISSED:");
      Serial.println(activeAlerts[i].fullName);
    }
  }

  updateDisplay("DOSE MISSED :(", missedList.length() > 0 ? missedList : "Timeout");
  scheduleIdleScreen(5000);
  alertCount = 0;

  Serial.println("[ARDUINO] ⌛ 20-Minute Grace Period Expired: Marked MISSED.");
}

// ── Non-Blocking Manual Dispense Pulse (5 Seconds) ───────────────────────────
void triggerDispenseSolenoid(unsigned long durationMs = 5000, int drawerNum = 0) {
  Serial.print("[ARDUINO] Manual Solenoid Relay Unlock Triggered for Drawer ");
  Serial.println(drawerNum == 0 ? "1 & 2" : String(drawerNum));

  updateDisplay("UNLOCKED (5s)", drawerNum == 0 ? "Drw 1 & 2 Relays" : ("Drw " + String(drawerNum) + " Relay"));

  if (drawerNum == 1 || drawerNum == 0) {
    digitalWrite(SOLENOID_DRAWER1_PIN, SOLENOID_ON);
    solenoid1OffTime = millis() + durationMs;
    solenoid1PulseActive = true;
  }
  if (drawerNum == 2 || drawerNum == 0) {
    digitalWrite(SOLENOID_DRAWER2_PIN, SOLENOID_ON);
    solenoid2OffTime = millis() + durationMs;
    solenoid2PulseActive = true;
  }

  digitalWrite(BUZZER_RELAY_PIN, RELAY_ON);
  buzzerPulseOffTime = millis() + 400; // 400ms chirp
  buzzerPulseActive = true;

  scheduleIdleScreen(durationMs + 1000);
}

// ── Non-Blocking Buzzer Pulse (2 Seconds) ────────────────────────────────────
void triggerBuzzerRelay(unsigned long durationMs = 2000) {
  Serial.println("[ARDUINO] Buzzer Test Sounding (2s)");
  updateDisplay("BUZZER TEST", "Active 2s...");

  digitalWrite(BUZZER_RELAY_PIN, RELAY_ON);
  buzzerPulseOffTime = millis() + durationMs;
  buzzerPulseActive = true;

  scheduleIdleScreen(durationMs + 500);
}

#if HAS_WIFI
// ── UDP Automatic Discovery Handler for Arduino Uno R4 WiFi ───────────────────
void handleUDPDiscovery() {
  if (WiFi.status() != WL_CONNECTED) return;
  unsigned long now = millis();

  // 1. Periodically broadcast discovery beacon every 3 seconds to LAN
  if (now - lastDiscoveryBeacon >= 3000) {
    lastDiscoveryBeacon = now;
    udpDiscovery.beginPacket(IPAddress(255, 255, 255, 255), UDP_DISCOVERY_PORT);
    udpDiscovery.print("DISPENSER:ARDUINO_R4:80");
    udpDiscovery.endPacket();
  }

  // 2. Listen for UDP beacons from ESP32-Mini, ESP1, ESP2, or Mobile app
  int packetSize = udpDiscovery.parsePacket();
  if (packetSize > 0) {
    char buf[64];
    int len = udpDiscovery.read(buf, sizeof(buf) - 1);
    if (len > 0) {
      buf[len] = '\0';
      String msg = String(buf);
      msg.trim();
      IPAddress senderIP = udpDiscovery.remoteIP();

      if (msg.startsWith("DISPENSER:ESP32_MASTER")) {
        if (esp32IP != senderIP) {
          esp32IP = senderIP;
          Serial.print("[UDP] Discovered ESP32 Master IP: ");
          Serial.println(esp32IP);
        }
      } else if (msg.startsWith("DISPENSER:ESP1")) {
        if (esp1IP != senderIP) {
          esp1IP = senderIP;
          Serial.print("[UDP] Discovered ESP1 (Drawer 1) IP: ");
          Serial.println(esp1IP);
        }
      } else if (msg.startsWith("DISPENSER:ESP2")) {
        if (esp2IP != senderIP) {
          esp2IP = senderIP;
          Serial.print("[UDP] Discovered ESP2 (Drawer 2) IP: ");
          Serial.println(esp2IP);
        }
      } else if (msg.startsWith("DISPENSER_DISCOVER")) {
        // Direct reply to Mobile App or querying device
        udpDiscovery.beginPacket(senderIP, udpDiscovery.remotePort());
        udpDiscovery.print("DISPENSER:ARDUINO_R4:80");
        udpDiscovery.endPacket();
      }
    }
  }
}
#endif

// ── Notify ESP1 / ESP2 and ESP32-Mini of Drawer Open/Close State ──────────────
void notifyDrawerDoor(int drawer, bool isOpen) {
  // 1. Output to Serial for debug / USB monitor
  Serial.print("DRAWER:");
  Serial.print(drawer);
  Serial.print(":");
  Serial.println(isOpen ? "OPEN" : "CLOSE");

#if HAS_WIFI
  if (WiFi.status() == WL_CONNECTED) {
    WiFiClient cli;
    bool connected = false;
    String hostStr = "";

    // 1. Prefer auto-discovered IP from UDP beacons (Zero-config)
    if (drawer == 1 && esp1IP != INADDR_NONE) {
      connected = cli.connect(esp1IP, 80);
      hostStr = esp1IP.toString();
    } else if (drawer == 2 && esp2IP != INADDR_NONE) {
      connected = cli.connect(esp2IP, 80);
      hostStr = esp2IP.toString();
    }

    // 2. Fallback to mDNS hostname if IP not yet learned via UDP
    if (!connected) {
      const char* host = (drawer == 1) ? "dispenser-drawer1.local" : "dispenser-drawer2.local";
      hostStr = String(host);
      connected = cli.connect(host, 80);
    }

    if (connected) {
      cli.print(String("GET /door?state=") + (isOpen ? "open" : "closed") + " HTTP/1.1\r\n" +
                "Host: " + hostStr + "\r\n" +
                "Connection: close\r\n\r\n");
      delay(10);
      cli.stop();
    }
  }
#endif
}

// ── Monitor Magnetic Reed Switches ───────────────────────────────────────────
void checkMagneticReedSwitches() {
  int r1 = digitalRead(REED_SWITCH_DRAWER1);
  int r2 = digitalRead(REED_SWITCH_DRAWER2);

  unsigned long now = millis();

  // 1. Debounce Reed Switch 1 (Drawer 1 -> Controls ESP1 LEDs)
  if (r1 != lastReed1State) {
    lastDebounceTime1 = now;
  }
  if ((now - lastDebounceTime1) > DEBOUNCE_DELAY_MS) {
    // Magnet APART (Drawer opened) -> OPEN (Turn ON ESP1 LEDs!)
    // Magnet PUT (Drawer closed)   -> CLOSED (Turn OFF ESP1 LEDs!)
    bool currentD1Open = (r1 == HIGH);

    if (currentD1Open != drawer1IsOpen) {
      drawer1IsOpen = currentD1Open;
      if (drawer1IsOpen) {
        Serial.println("[REED] Drawer 1 OPENED (Magnet Apart) -> Notifying ESP1 to turn ON LEDs!");
        notifyDrawerDoor(1, true);
        if (isAlertActive) {
          // Patient opened drawer during alert: silence buzzer for retrieval
          buzzerState = false;
          digitalWrite(BUZZER_RELAY_PIN, RELAY_OFF);
          updateDisplay("DRAWER 1 OPEN", "Retrieve Dose :)");
        }
      } else {
        Serial.println("[REED] Drawer 1 CLOSED (Magnet Put) -> Notifying ESP1 to turn OFF LEDs!");
        notifyDrawerDoor(1, false);
        lockDrawer(1); // Relock solenoid immediately when drawer is closed
        if (isAlertActive) {
          // Drawer was opened and is now pushed closed -> Dose complete!
          resolveDoseTaken(1);
        }
      }
    }
  }
  lastReed1State = r1;

  // 2. Debounce Reed Switch 2 (Drawer 2 -> Controls ESP2 LEDs)
  if (r2 != lastReed2State) {
    lastDebounceTime2 = now;
  }
  if ((now - lastDebounceTime2) > DEBOUNCE_DELAY_MS) {
    // Magnet APART (Drawer opened) -> OPEN (Turn ON ESP2 LEDs!)
    // Magnet PUT (Drawer closed)   -> CLOSED (Turn OFF ESP2 LEDs!)
    bool currentD2Open = (r2 == HIGH);

    if (currentD2Open != drawer2IsOpen) {
      drawer2IsOpen = currentD2Open;
      if (drawer2IsOpen) {
        Serial.println("[REED] Drawer 2 OPENED (Magnet Apart) -> Notifying ESP2 to turn ON LEDs!");
        notifyDrawerDoor(2, true);
        if (isAlertActive) {
          buzzerState = false;
          digitalWrite(BUZZER_RELAY_PIN, RELAY_OFF);
          updateDisplay("DRAWER 2 OPEN", "Retrieve Dose :)");
        }
      } else {
        Serial.println("[REED] Drawer 2 CLOSED (Magnet Put) -> Notifying ESP2 to turn OFF LEDs!");
        notifyDrawerDoor(2, false);
        lockDrawer(2); // Relock solenoid immediately when drawer is closed
        if (isAlertActive) {
          resolveDoseTaken(2);
        }
      }
    }
  }
  lastReed2State = r2;

  // 3. Local Indicator LED Control: Will NOT lit up if reed is closed; ONLY if open!
  bool anyDrawerOpen = drawer1IsOpen || drawer2IsOpen;
  digitalWrite(STATUS_LED_PIN, anyDrawerOpen ? HIGH : LOW);
}

// ── Process Serial Commands from ESP32-Mini ──────────────────────────────────
void processSerialCommand(const String& cmd) {
  // Format: ALERT:<compartment> (e.g. "ALERT:Drawer 1 (B1)" or "ALERT:B1, B2, B3")
  if (cmd.startsWith("ALERT:")) {
    String comp = urlDecode(cmd.substring(6));
    comp.trim();
    String upperComp = comp;
    upperComp.toUpperCase();
    int drawer = 1;
    if (upperComp.indexOf("DRAWER 2") != -1 || upperComp.indexOf("2:") == 0 || upperComp.indexOf("(2") != -1) {
      drawer = 2;
    }
    startAlert(comp, drawer);
  }
  else if (cmd.startsWith("DISPENSE")) {
    int drw = 1;
    if (cmd.indexOf(":2") != -1) drw = 2;
    else if (cmd.indexOf(":1") != -1) drw = 1;
    triggerDispenseSolenoid(5000, drw);
  }
  else if (cmd.startsWith("SOLENOID")) {
    int drw = 1;
    if (cmd.indexOf(":2") != -1) drw = 2;
    else if (cmd.indexOf(":1") != -1) drw = 1;
    else if (cmd.indexOf(":0") != -1) drw = 0;
    if (cmd.indexOf("UNLOCK") != -1 || cmd.indexOf("OPEN") != -1) {
      unlockDrawer(drw);
    } else {
      lockDrawer(drw);
    }
  }
  else if (cmd.startsWith("BUZZER") || cmd.startsWith("RELAY") || cmd.startsWith("BEEP")) {
    triggerBuzzerRelay(2000);
  }
  else if (cmd.startsWith("TAKEN")) {
    String comp = cmd.length() > 6 ? cmd.substring(6) : "";
    int drw = 1;
    if (comp.indexOf("Drawer 2") != -1 || comp.indexOf("DRAWER 2") != -1) drw = 2;
    resolveDoseTaken(drw);
  }
  else if (cmd.startsWith("MISSED")) {
    resolveDoseMissed();
  }
  else if (cmd == "STOP" || cmd == "SILENCE" || cmd == "CLEAR") {
    isAlertActive = false;
    alertCount = 0;
    digitalWrite(BUZZER_RELAY_PIN, RELAY_OFF);
    lockDrawer(0); // Relock both solenoid relays
    buzzerState = false;
    updateDisplay("Alert Stopped", "Smart Dispenser");
    scheduleIdleScreen(2000);
  }
  else if (cmd.startsWith("IP:")) {
    deviceIPStr = cmd.substring(3);
    deviceIPStr.trim();
    if (!isAlertActive) {
      updateDisplay("WiFi Connected!", deviceIPStr);
      scheduleIdleScreen(5000);
    }
  }
  else if (cmd.startsWith("TIME:")) {
    String timeStr = cmd.substring(5);
    timeStr.trim();
    if (!isAlertActive) {
      updateDisplay("Smart Dispenser", timeStr);
      scheduleIdleScreen(4000);
    }
  }
  else if (cmd == "PING" || cmd == "STATUS" || cmd == "REED") {
    Serial.println("PONG:OK");
    Serial.print("[STATUS] Reed 1 Raw (Pin 2): ");
    Serial.print(digitalRead(REED_SWITCH_DRAWER1));
    Serial.print(" -> Drawer 1: ");
    Serial.print(drawer1IsOpen ? "OPEN (LEDs ON)" : "CLOSED (LEDs OFF)");
    Serial.print(" | Reed 2 Raw (Pin 3): ");
    Serial.print(digitalRead(REED_SWITCH_DRAWER2));
    Serial.print(" -> Drawer 2: ");
    Serial.print(drawer2IsOpen ? "OPEN (LEDs ON)" : "CLOSED (LEDs OFF)");
    Serial.println();
    if (!isAlertActive) {
      updateDisplay("Master Linked", "ESP32 Connected");
      scheduleIdleScreen(2000);
    }
  }

}

// ── Check Scheduled Alarms on Arduino (Direct Wi-Fi / Uno R4) ────────────────
void checkArduinoSchedules() {
  if (clientBaseEpoch == 0 || scheduleCount == 0) return;

  unsigned long curEpoch = clientBaseEpoch + ((millis() - clientBaseMillis) / 1000);
  unsigned long localEpoch = curEpoch + 28800UL; // GMT+8
  int curHour = (localEpoch % 86400) / 3600;
  int curMin  = (localEpoch % 3600) / 60;

  for (int i = 0; i < scheduleCount; i++) {
    if (schedules[i].active && schedules[i].hour == curHour && schedules[i].minute == curMin) {
      if (!schedules[i].alertedToday) {
        schedules[i].alertedToday = true;
        Serial.print("[ARDUINO] 🚨 SCHEDULE DUE! Ringing for: ");
        Serial.println(schedules[i].compartment);
        startAlert(schedules[i].compartment, schedules[i].drawer);
      }
    } else if (schedules[i].minute != curMin) {
      schedules[i].alertedToday = false;
    }
  }
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

// ── Setup ────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  Serial.setTimeout(50); // Fast timeout to prevent blocking on partial serial lines

  // 1. Initialize Buzzer Relay Pin (GPIO 7 - Active-LOW)
  digitalWrite(BUZZER_RELAY_PIN, RELAY_OFF);
  pinMode(BUZZER_RELAY_PIN, OUTPUT);

  // 2. Initialize Dual Solenoid Relay Pins (Pin 8 for Drawer 1, Pin 9 for Drawer 2 - Active-LOW)
  digitalWrite(SOLENOID_DRAWER1_PIN, SOLENOID_OFF);
  pinMode(SOLENOID_DRAWER1_PIN, OUTPUT);

  digitalWrite(SOLENOID_DRAWER2_PIN, SOLENOID_OFF);
  pinMode(SOLENOID_DRAWER2_PIN, OUTPUT);

  // 3. Initialize Status LED (GPIO 13)
  pinMode(STATUS_LED_PIN, OUTPUT);
  digitalWrite(STATUS_LED_PIN, LOW);

  // 4. Initialize Magnetic Reed Switches (Pins 2 & 3 with internal pull-up)
  pinMode(REED_SWITCH_DRAWER1, INPUT_PULLUP);
  pinMode(REED_SWITCH_DRAWER2, INPUT_PULLUP);
  delay(50);

  // Read actual sensor levels at startup; never assume drawers are closed.
  lastReed1State = digitalRead(REED_SWITCH_DRAWER1);
  lastReed2State = digitalRead(REED_SWITCH_DRAWER2);
  drawer1IsOpen = (lastReed1State == HIGH);
  drawer2IsOpen = (lastReed2State == HIGH);
  digitalWrite(STATUS_LED_PIN, (drawer1IsOpen || drawer2IsOpen) ? HIGH : LOW);
  Serial.print("[REED] Drawer 1: ");
  Serial.print(drawer1IsOpen ? "OPEN" : "CLOSED");
  Serial.print(" | Drawer 2: ");
  Serial.println(drawer2IsOpen ? "OPEN" : "CLOSED");

  // 5. Initialize I2C LCD Display (Auto-detect address 0x27 or 0x3F)
  Wire.begin();
  Wire.setClock(100000); // Standard 100kHz I2C clock for glitch-free LCD communication
  byte lcdAddr = 0x27;
  Wire.beginTransmission(0x27);
  if (Wire.endTransmission() != 0) {
    Wire.beginTransmission(0x3F);
    if (Wire.endTransmission() == 0) {
      lcdAddr = 0x3F;
    }
  }
  lcd = LiquidCrystal_I2C(lcdAddr, 16, 2);
  lcd.init();
  lcd.backlight();
  updateDisplay("Smart Dispenser", "Initializing...");

  delay(600);

#if HAS_WIFI
  Serial.print("[ARDUINO] Initializing WiFi for SSID '");
  Serial.print(ssid);
  Serial.println("'...");
  updateDisplay("Connecting WiFi", String(ssid).substring(0, 16));

  WiFi.disconnect();
  delay(150);

  int attempt = 0;
  while (WiFi.status() != WL_CONNECTED && attempt < 4) {
    attempt++;
    Serial.print("[ARDUINO] WiFi Attempt ");
    Serial.print(attempt);
    Serial.print("/4... ");
    WiFi.begin(ssid, pass);

    int waitDots = 0;
    while (WiFi.status() != WL_CONNECTED && waitDots < 12) {
      delay(400);
      Serial.print(".");
      waitDots++;
    }
    Serial.print(" Result: ");
    Serial.println(wifiStatusToString(WiFi.status()));
    if (WiFi.status() == WL_CONNECTED) break;
    delay(400);
  }

  if (WiFi.status() == WL_CONNECTED) {
    server.begin();
    udpDiscovery.begin(UDP_DISCOVERY_PORT);
    deviceIPStr = WiFi.localIP().toString();
    updateDisplay("WiFi Connected!", deviceIPStr);
    Serial.print("[ARDUINO] ✅ WiFi Connected! IP: ");
    Serial.println(deviceIPStr);
    Serial.print("[ARDUINO] 🔍 UDP Auto-Discovery Beacon active on port ");
    Serial.println(UDP_DISCOVERY_PORT);
    delay(1500);
  } else {
    updateDisplay("WiFi Offline", "Serial Link ON");
    delay(1000);
  }
#else
  Serial.println("[ARDUINO] Standard Serial-Slave Mode (Controlled via UART Pin 0/RX & Pin 1/TX).");
#endif

  showIdleScreen();
  Serial.println("[ARDUINO] Ready! Dual Solenoid Relays (Pin 8 & 9) + Reed Switches (Pin 2 & 3).");
}

// ── Loop ─────────────────────────────────────────────────────────────────────
void loop() {
  // 1. Process Serial Commands from ESP32-Mini Master
  if (Serial.available() > 0) {
    String command = Serial.readStringUntil('\n');
    command.trim();
    if (command.length() > 0) {
      processSerialCommand(command);
    }
  }

  // 2. Monitor Magnetic Reed Switches (Detects physical drawer retrieval)
  checkMagneticReedSwitches();

  // 3. Active 20-Minute Alert State Machine & 2-Minute Buzzer Cutoff
  if (isAlertActive) {
    unsigned long elapsed = millis() - alertStartTime;

    // A. Check 20-minute grace period expiry (Timeout -> MISSED)
    if (elapsed >= GRACE_PERIOD_MS) {
      resolveDoseMissed();
    } else {
      unsigned long now = millis();

      // B. Buzzer Beeps ONLY for the first 2 minutes (120,000 ms)
      if (elapsed < BUZZER_DURATION_MS) {
        if (buzzerState && (now - lastBuzzerToggle >= BUZZER_BEEP_MS)) {
          buzzerState = false;
          digitalWrite(BUZZER_RELAY_PIN, RELAY_OFF);
          lastBuzzerToggle = now;
        } else if (!buzzerState && (now - lastBuzzerToggle >= BUZZER_PAUSE_MS)) {
          buzzerState = true;
          digitalWrite(BUZZER_RELAY_PIN, RELAY_ON);
          lastBuzzerToggle = now;
        }
      } else {
        // 2-minute buzzer cutoff reached -> keep buzzer quiet
        if (buzzerState) {
          buzzerState = false;
          digitalWrite(BUZZER_RELAY_PIN, RELAY_OFF);
          Serial.println("[ARDUINO] 2-Minute Buzzing Timeout: Buzzer silenced. Alert & countdown continuing on LCD.");
        }
      }

      // C. Line 1: Increment / Cycle through multiple scheduled compartments every 2 seconds
      bool shouldUpdateLine1 = false;
      if (now - lastDisplayCycleMs >= DISPLAY_CYCLE_INTERVAL_MS || lastDisplayCycleMs == 0) {
        lastDisplayCycleMs = now;
        currentDisplayIdx++;
        shouldUpdateLine1 = true;
      }

      // Count remaining untaken compartments
      int untakenCount = 0;
      for (int i = 0; i < alertCount; i++) {
        if (!activeAlerts[i].taken) untakenCount++;
      }

      if (untakenCount > 0 && shouldUpdateLine1) {
        int cycleStates = (untakenCount > 1) ? (untakenCount + 1) : 1;
        int state = currentDisplayIdx % cycleStates;

        String line1 = "";
        if (state == untakenCount && untakenCount > 1) {
          // Combined Summary Slide: e.g. "DUE: B1, B2, B3"
          line1 = "DUE: ";
          int added = 0;
          for (int i = 0; i < alertCount; i++) {
            if (!activeAlerts[i].taken) {
              if (added > 0) line1 += ", ";
              line1 += activeAlerts[i].shortLabel;
              added++;
            }
          }
        } else {
          // Individual Compartment Slide: e.g. "DUE: B1 (1/3)" -> "DUE: B2 (2/3)"
          int current = 0;
          for (int i = 0; i < alertCount; i++) {
            if (!activeAlerts[i].taken) {
              if (current == (state % untakenCount)) {
                if (untakenCount > 1) {
                  line1 = "DUE: " + activeAlerts[i].shortLabel + " (" + String(current + 1) + "/" + String(untakenCount) + ")";
                } else {
                  line1 = "DUE: " + activeAlerts[i].shortLabel + " (Drw " + String(activeAlerts[i].drawer) + ")";
                }
                break;
              }
              current++;
            }
          }
        }

        // Print Line 1 cleanly with space padding
        writeLCDLine(0, line1);
      }

      // D. Line 2: 20-Minute Countdown Display (Updated once per second)
      if (now - lastCountdownUpdate >= 1000) {
        lastCountdownUpdate = now;
        unsigned long remainingMs = GRACE_PERIOD_MS - elapsed;
        int remSecTotal = remainingMs / 1000;
        int remMin = remSecTotal / 60;
        int remSec = remSecTotal % 60;

        char timeBuf[17];
        snprintf(timeBuf, sizeof(timeBuf), "%02dm %02ds left", remMin, remSec);
        writeLCDLine(1, String(timeBuf));
      }
    }
  }

  // 4. Handle Independent Non-Blocking Solenoid Relock Pulses
  if (solenoid1PulseActive && millis() >= solenoid1OffTime) {
    bool drw1StillAlerting = false;
    if (isAlertActive) {
      for (int i = 0; i < alertCount; i++) {
        if (!activeAlerts[i].taken && activeAlerts[i].drawer == 1) {
          drw1StillAlerting = true;
          break;
        }
      }
    }
    if (!drw1StillAlerting) {
      digitalWrite(SOLENOID_DRAWER1_PIN, SOLENOID_OFF);
      solenoid1PulseActive = false;
      Serial.println("[ARDUINO] Solenoid Relay 1 (Drawer 1 / Pin 8) Relocked.");
    }
  }

  if (solenoid2PulseActive && millis() >= solenoid2OffTime) {
    bool drw2StillAlerting = false;
    if (isAlertActive) {
      for (int i = 0; i < alertCount; i++) {
        if (!activeAlerts[i].taken && activeAlerts[i].drawer == 2) {
          drw2StillAlerting = true;
          break;
        }
      }
    }
    if (!drw2StillAlerting) {
      digitalWrite(SOLENOID_DRAWER2_PIN, SOLENOID_OFF);
      solenoid2PulseActive = false;
      Serial.println("[ARDUINO] Solenoid Relay 2 (Drawer 2 / Pin 9) Relocked.");
    }
  }

  // 5. Handle Non-Blocking Buzzer Pulse (Manual tests)
  if (buzzerPulseActive && !isAlertActive && millis() >= buzzerPulseOffTime) {
    buzzerPulseActive = false;
    digitalWrite(BUZZER_RELAY_PIN, RELAY_OFF);
  }

  // 6. LCD Auto-Reset to Idle Screen
  if (!isAlertActive && lcdNeedsReset && millis() >= lcdResetTime) {
    lcdNeedsReset = false;
    showIdleScreen();
  }

  // 7. Check Internal Schedules (Uno R4 WiFi)
  checkArduinoSchedules();

#if HAS_WIFI
  // 8. Auto-Discovery UDP beacon & listener for dynamic IP synchronization
  handleUDPDiscovery();

  // 9. Direct HTTP Server for Uno R4 WiFi
  WiFiClient client = server.available();
  if (client) {
    String req = client.readStringUntil('\r');
    client.flush();

    if (req.indexOf("/set-schedule") != -1) {
      int h = getQueryParam(req, "hour").toInt();
      int m = getQueryParam(req, "minute").toInt();
      String amPm = getQueryParam(req, "amPm");
      String rawComp = getQueryParam(req, "compartment");
      String comp = urlDecode(rawComp);
      comp.trim();
      String epochStr = getQueryParam(req, "client_epoch");
      int drwParam = getQueryParam(req, "drawer").toInt();
      int slotParam = getQueryParam(req, "slotIndex").length() > 0 ? getQueryParam(req, "slotIndex").toInt() : -1;

      String upperComp = comp;
      upperComp.toUpperCase();
      int drw = 1;
      if (drwParam == 2 || slotParam >= 21 || upperComp.indexOf("DRAWER 2") != -1 || upperComp.indexOf("2:") == 0 || upperComp.indexOf("(2") != -1) {
        drw = 2;
      }

      // 12h to 24h
      int h24 = h;
      if (amPm.equalsIgnoreCase("AM") && h == 12) h24 = 0;
      else if (amPm.equalsIgnoreCase("PM") && h != 12) h24 = h + 12;

      if (epochStr.length() > 0) {
        clientBaseEpoch = (unsigned long)epochStr.toInt();
        clientBaseMillis = millis();
      }

      // Calculate current hour/minute to suppress immediate false alarm if schedule matches current time
      unsigned long curEpoch = clientBaseEpoch + ((millis() - clientBaseMillis) / 1000);
      unsigned long localEpoch = curEpoch + 28800UL; // GMT+8
      int curHour = (localEpoch % 86400) / 3600;
      int curMin  = (localEpoch % 3600) / 60;
      bool isNow = (clientBaseEpoch > 0 && h24 == curHour && m == curMin);

      bool updated = false;
      for (int i = 0; i < scheduleCount; i++) {
        if (schedules[i].compartment == comp) {
          schedules[i].hour = h24;
          schedules[i].minute = m;
          schedules[i].drawer = drw;
          schedules[i].alertedToday = isNow; // Suppress immediate trigger if saved during current minute
          schedules[i].active = true;
          updated = true;
          break;
        }
      }
      if (!updated && scheduleCount < MAX_ARDUINO_SCHEDULES) {
        schedules[scheduleCount].hour = h24;
        schedules[scheduleCount].minute = m;
        schedules[scheduleCount].compartment = comp.length() > 0 ? comp : (drw == 2 ? "Drawer 2 (A1)" : "Drawer 1 (A1)");
        schedules[scheduleCount].drawer = drw;
        schedules[scheduleCount].alertedToday = isNow; // Suppress immediate trigger if saved during current minute
        schedules[scheduleCount].active = true;
        scheduleCount++;
      }

      char dispTime[17];
      snprintf(dispTime, sizeof(dispTime), "%02d:%02d %s", h, m, amPm.c_str());
      updateDisplay("Schedule Saved!", String(dispTime));
      scheduleIdleScreen(3000);

      client.println("HTTP/1.1 200 OK");
      client.println("Content-Type: application/json");
      client.println("Connection: close");
      client.println("Access-Control-Allow-Origin: *");
      client.println();
      client.println("{\"status\":\"ok\",\"drawer\":" + String(drw) + "}");
      delay(10);
      client.stop();
    }
    else if (req.indexOf("/alert") != -1) {
      String rawComp = getQueryParam(req, "compartment");
      String comp = urlDecode(rawComp);
      comp.trim();
      int drwParam = getQueryParam(req, "drawer").toInt();
      int slotParam = getQueryParam(req, "slotIndex").length() > 0 ? getQueryParam(req, "slotIndex").toInt() : -1;

      String upperComp = comp;
      upperComp.toUpperCase();
      int drw = 1;
      if (drwParam == 2 || slotParam >= 21 || upperComp.indexOf("DRAWER 2") != -1 || upperComp.indexOf("2:") == 0 || upperComp.indexOf("(2") != -1) {
        drw = 2;
      }
      if (comp.length() == 0) comp = (drw == 2 ? "Drawer 2 (A1)" : "Drawer 1 (A1)");

      client.println("HTTP/1.1 200 OK");
      client.println("Content-Type: application/json");
      client.println("Connection: close");
      client.println("Access-Control-Allow-Origin: *");
      client.println();
      client.println("{\"status\":\"ok\",\"action\":\"alert_started\",\"drawer\":" + String(drw) + "}");
      delay(10);
      client.stop();

      startAlert(comp, drw);
    }
    else if (req.indexOf("/dispense") != -1) {
      int relayNum = getQueryParam(req, "relay").toInt();
      int drw = 0;
      if (relayNum == 1 || relayNum == 2) drw = 1;
      else if (relayNum == 3 || relayNum == 4) drw = 2;
      String drwArg = getQueryParam(req, "drawer");
      if (drwArg.length() > 0) drw = drwArg.toInt();

      client.println("HTTP/1.1 200 OK");
      client.println("Content-Type: application/json");
      client.println("Connection: close");
      client.println("Access-Control-Allow-Origin: *");
      client.println();
      client.println("{\"status\":\"ok\",\"drawer\":" + String(drw) + "}");
      delay(10);
      client.stop();

      triggerDispenseSolenoid(5000, drw);
    }
    else if (req.indexOf("/solenoid") != -1) {
      int drw = getQueryParam(req, "drawer").toInt();
      String state = getQueryParam(req, "state");
      bool unlock = (state.equalsIgnoreCase("unlock") || state.equalsIgnoreCase("open") || state == "1");

      if (unlock) {
        unlockDrawer(drw);
      } else {
        lockDrawer(drw);
      }

      client.println("HTTP/1.1 200 OK");
      client.println("Content-Type: application/json");
      client.println("Connection: close");
      client.println("Access-Control-Allow-Origin: *");
      client.println();
      client.println("{\"status\":\"ok\",\"action\":\"solenoid\",\"drawer\":" + String(drw) + ",\"unlocked\":" + String(unlock ? "true" : "false") + "}");
      delay(10);
      client.stop();
    }
    else if (req.indexOf("/buzzer") != -1 || req.indexOf("/relay") != -1) {
      client.println("HTTP/1.1 200 OK");
      client.println("Content-Type: application/json");
      client.println("Connection: close");
      client.println("Access-Control-Allow-Origin: *");
      client.println();
      client.println("{\"status\":\"ok\",\"action\":\"buzzer_2s\"}");
      delay(10);
      client.stop();

      triggerBuzzerRelay(2000);
    }
    else if (req.indexOf("/stop-vibrate") != -1 || req.indexOf("/confirm") != -1) {
      client.println("HTTP/1.1 200 OK");
      client.println("Content-Type: application/json");
      client.println("Connection: close");
      client.println("Access-Control-Allow-Origin: *");
      client.println();
      client.println("{\"status\":\"ok\",\"action\":\"stopped\"}");
      delay(10);
      client.stop();

      resolveDoseTaken(0); // Resolve all drawers
    }
    else if (req.indexOf("/status") != -1 || req.indexOf("/ping") != -1) {
      String json = "{\"status\":\"online\",\"device\":\"Arduino-Controller\",\"is_alerting\":" + String(isAlertActive ? "true" : "false") + ",\"active_alerts\":" + String(alertCount) + ",\"schedules\":" + String(scheduleCount) + ",\"last_dose_taken\":" + String(lastDoseTaken ? "true" : "false") + ",\"last_taken_ms_ago\":" + String(lastDoseTaken ? (millis() - lastDoseTakenMillis) : 999999) + ",\"last_taken_comp\":\"" + lastTakenSummary + "\"";
      json += ",\"esp32_ip\":\"" + (esp32IP != INADDR_NONE ? esp32IP.toString() : "") + "\"";
      json += ",\"esp1_ip\":\"" + (esp1IP != INADDR_NONE ? esp1IP.toString() : "") + "\"";
      json += ",\"esp2_ip\":\"" + (esp2IP != INADDR_NONE ? esp2IP.toString() : "") + "\"}";
      client.println("HTTP/1.1 200 OK");
      client.println("Content-Type: application/json");
      client.println("Connection: close");
      client.println("Access-Control-Allow-Origin: *");
      client.println();
      client.println(json);
      delay(10);
      client.stop();
    }
    else if (req.startsWith("OPTIONS")) {
      client.println("HTTP/1.1 204 No Content");
      client.println("Access-Control-Allow-Origin: *");
      client.println("Access-Control-Allow-Methods: GET, POST, OPTIONS");
      client.println("Access-Control-Allow-Headers: *");
      client.println("Connection: close");
      client.println();
      delay(10);
      client.stop();
    }
    else {
      client.println("HTTP/1.1 404 Not Found");
      client.println("Connection: close");
      client.println();
      client.println("Not Found");
      delay(10);
      client.stop();
    }
  }
#endif
}