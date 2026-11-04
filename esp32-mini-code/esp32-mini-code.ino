#include <WiFi.h>
#include <WiFiUdp.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <time.h>
#include <Preferences.h>
#include <HTTPClient.h>

// ==============================================================================
//  SMART MEDICINE DISPENSER - ESP32 MINI MASTER CONTROLLER
//  Firmware Version: 2.1 (Full 20-Min Grace Period + Auto-Discovery UDP + Wi-Fi)
// ==============================================================================

// ── Wi-Fi Configuration ──────────────────────────────────────────────────────
const char* ssid     = "Hi";            // <-- 2.4G Wi-Fi SSID
const char* password = "estogs000";     // <-- 2.4G Wi-Fi password

// ── NTP & Timezone Configuration ─────────────────────────────────────────────
// Default timezone: GMT+8 (Philippines / Asia/Manila)
long gmtOffset_sec     = 8 * 3600; 
int daylightOffset_sec = 0;
const char* ntpServer1 = "pool.ntp.org";
const char* ntpServer2 = "time.nist.gov";

// ── Drawer ESP & Arduino Hostnames / Auto-Discovered Endpoints ───────────────
const char* DRAWER1_HOST = "http://dispenser-drawer1.local";
const char* DRAWER2_HOST = "http://dispenser-drawer2.local";
const char* ARDUINO_HOST = "http://10.146.105.91";

// Dynamic Auto-Discovered Hosts (learned via UDP Broadcasts)
String dynamicArduinoHost = "";
String dynamicDrawer1Host = "";
String dynamicDrawer2Host = "";
WiFiUDP udpDiscovery;
const unsigned int UDP_DISCOVERY_PORT = 4210;
unsigned long lastDiscoveryBeacon = 0;

// ── Pin Configuration ────────────────────────────────────────────────────────
#if defined(CONFIG_IDF_TARGET_ESP32C3) || defined(ARDUINO_ESP32C3_DEV) || defined(ARDUINO_ESP32C3)
  // ESP32-C3 SuperMini Pinout
  #define VIBRATION_PIN    4    // GPIO 4 (Vibration Motor control pin)
  #define STATUS_LED_PIN   8    // GPIO 8 (Onboard Blue LED, Active LOW)
  #define LED_ACTIVE       LOW
  #define LED_INACTIVE     HIGH
  #define BUZZER_PIN       -1   // Set to GPIO or -1 to disable
  #define RELAY1_PIN       0
  #define RELAY2_PIN       1
  #define RELAY3_PIN       2
  #define RELAY4_PIN       3
#else
  // Classic ESP32 / D1 Mini Pinout
  #define VIBRATION_PIN    4    // GPIO 4 (Vibration Motor control pin)
  #define STATUS_LED_PIN   2    // GPIO 2 (Built-in Blue LED, Active HIGH)
  #define LED_ACTIVE       HIGH
  #define LED_INACTIVE     LOW
  #define BUZZER_PIN       -1
  #define RELAY1_PIN       18
  #define RELAY2_PIN       19
  #define RELAY3_PIN       21
  #define RELAY4_PIN       -1
#endif

// Active Levels
#define VIBRATION_ACTIVE HIGH   // HIGH = Motor ON (via NPN transistor or driver module)
#define VIBRATION_INACTIVE LOW
#define RELAY_ON         LOW    // Active LOW relays
#define RELAY_OFF        HIGH

// ── Web Server & Flash Storage ───────────────────────────────────────────────
WebServer server(80);
Preferences preferences;

// ── Schedule & Event Data Structures ─────────────────────────────────────────
#define MAX_SCHEDULES  28
#define MAX_EVENTS     20
#define MAX_BULK_SLOTS 42
#define GRACE_PERIOD_SECONDS 1200 // 20 minutes countdown window

struct ReminderSchedule {
  char id[32];
  int hour;             // 24-hour format: 0 - 23
  int minute;           // 0 - 59
  char date[16];        // "YYYY-MM-DD" or "" for repeating daily
  char compartment[224]; // e.g. "Drawer 1 (A1)" or "Drawer 1 (A1, A2, A3)"
  int slotIndex;        // 0 - 41
  bool active;
  unsigned long lastTriggeredDate; // YYYYMMDD, persisted to prevent replay after reboot.
  char slots[128];       // Comma-separated slot indices e.g. "0,1,2"
};

ReminderSchedule schedules[MAX_SCHEDULES];
int scheduleCount = 0;

struct DoseEventLog {
  char eventId[32];
  char scheduleId[32];
  char compartment[224];
  char triggerTime[24];
  char resolvedTime[24];
  char resolution[16];  // "TAKEN", "TAKEN_MANUAL", "MISSED"
  int elapsedSeconds;
};

DoseEventLog eventHistory[MAX_EVENTS];
int eventCount = 0;

// ── Active Alert State (20-Minute Countdown) ─────────────────────────────────
struct ActiveAlertState {
  bool isAlerting;
  char scheduleId[32];
  char compartment[224];
  int slotIndex;
  int relayNum;
  time_t alertStartEpoch;
  unsigned long lastReminderVibrateMs;
  int alertSlots[MAX_BULK_SLOTS];
  int alertSlotCount;
};

ActiveAlertState currentAlert = {
  false, "", "", 0, 1, 0, 0, {0}, 0
};

// ── Vibration Motor State Machine (Non-blocking) ─────────────────────────────
const unsigned long VIBRATION_ALERT_DURATION_MS = 60000UL; // 1 minute (60,000 ms) continuous vibration

bool isVibrating = false;
int currentPulse = 0;
int totalPulses = 1;
unsigned long pulseOnDuration = 60000UL;  // ms vibrating (default 1 minute)
unsigned long pulseOffDuration = 0;       // ms pause between pulses (0 for continuous)
unsigned long vibrationStateTimer = 0;
bool motorIsOn = false;

// Dispenser relay pulse state
unsigned long dispenseEndTime = 0;
int activeDispensePin = -1;

// Guard against duplicate triggers within the same minute
int lastTriggeredMinute = -1;
int lastTriggeredHour = -1;
int lastTriggeredDay = -1; // YYYYMMDD, not just the day of month.

// ── Helper: Format Current Time ──────────────────────────────────────────────
String getCurrentTimeString() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 50)) {
    return "Time Not Synced";
  }
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
  return String(buf);
}

time_t getCurrentEpoch() {
  time_t now;
  time(&now);
  return now;
}

// ── Vibration Alert Controller (Non-blocking) ────────────────────────────────
void startVibrationAlert(int pulses = 1, unsigned long onMs = 60000UL, unsigned long offMs = 0) {
  isVibrating = true;
  currentPulse = 0;
  totalPulses = pulses;
  pulseOnDuration = onMs;
  pulseOffDuration = offMs;

  motorIsOn = true;
  digitalWrite(VIBRATION_PIN, VIBRATION_ACTIVE);
  if (STATUS_LED_PIN >= 0) digitalWrite(STATUS_LED_PIN, LED_ACTIVE);
  if (BUZZER_PIN >= 0) digitalWrite(BUZZER_PIN, HIGH);

  vibrationStateTimer = millis();
  Serial.printf("[VIBRATION] Started: %d pulse(s) (%lu ms on / %lu ms off) - 1 minute continuous vibration active\n", pulses, onMs, offMs);
}

void stopVibration() {
  isVibrating = false;
  motorIsOn = false;
  digitalWrite(VIBRATION_PIN, VIBRATION_INACTIVE);
  if (STATUS_LED_PIN >= 0) digitalWrite(STATUS_LED_PIN, LED_INACTIVE);
  if (BUZZER_PIN >= 0) digitalWrite(BUZZER_PIN, LOW);
  Serial.println("[VIBRATION] Stopped.");
}

void updateVibrationState() {
  if (!isVibrating) return;

  unsigned long now = millis();

  if (motorIsOn) {
    if (now - vibrationStateTimer >= pulseOnDuration) {
      motorIsOn = false;
      digitalWrite(VIBRATION_PIN, VIBRATION_INACTIVE);
      if (STATUS_LED_PIN >= 0) digitalWrite(STATUS_LED_PIN, LED_INACTIVE);
      if (BUZZER_PIN >= 0) digitalWrite(BUZZER_PIN, LOW);
      vibrationStateTimer = now;
      currentPulse++;

      if (currentPulse >= totalPulses) {
        isVibrating = false;
        Serial.println("[VIBRATION] 1-minute vibration finished.");
      }
    }
  } else {
    if (pulseOffDuration > 0 && now - vibrationStateTimer >= pulseOffDuration) {
      motorIsOn = true;
      digitalWrite(VIBRATION_PIN, VIBRATION_ACTIVE);
      if (STATUS_LED_PIN >= 0) digitalWrite(STATUS_LED_PIN, LED_ACTIVE);
      if (BUZZER_PIN >= 0) digitalWrite(BUZZER_PIN, HIGH);
      vibrationStateTimer = now;
    }
  }
}

// ── Relay Control ────────────────────────────────────────────────────────────
void triggerDispenseRelay(int relayNum) {
  int pin = -1;
  switch (relayNum) {
    case 1: pin = RELAY1_PIN; break;
    case 2: pin = RELAY2_PIN; break;
    case 3: pin = RELAY3_PIN; break;
    case 4: pin = RELAY4_PIN; break;
    default: pin = -1; break;
  }

  if (pin >= 0) {
    digitalWrite(pin, RELAY_ON);
    activeDispensePin = pin;
    dispenseEndTime = millis() + 5000; // 5-second pulse
    Serial.printf("[DISPENSE] Relay %d (Pin %d) ON for 5s\n", relayNum, pin);
  }

  // Also bridge command to Arduino via Serial
  Serial.printf("DISPENSE:%d\n", relayNum);
}

void updateDispenserState() {
  if (activeDispensePin != -1 && millis() >= dispenseEndTime) {
    digitalWrite(activeDispensePin, RELAY_OFF);
    Serial.printf("[DISPENSE] Relay (Pin %d) OFF\n", activeDispensePin);
    activeDispensePin = -1;
  }
}

// ── Event History Logging ────────────────────────────────────────────────────
void logDoseEvent(const char* scheduleId, const char* compartment, const char* resolution, int elapsedSec) {
  // Shift existing events down if buffer full
  if (eventCount >= MAX_EVENTS) {
    for (int i = MAX_EVENTS - 1; i > 0; i--) {
      eventHistory[i] = eventHistory[i - 1];
    }
    eventCount = MAX_EVENTS - 1;
  } else {
    for (int i = eventCount; i > 0; i--) {
      eventHistory[i] = eventHistory[i - 1];
    }
  }

  snprintf(eventHistory[0].eventId, sizeof(eventHistory[0].eventId), "evt_%lu", (unsigned long)getCurrentEpoch());
  strncpy(eventHistory[0].scheduleId, scheduleId, sizeof(eventHistory[0].scheduleId));
  strncpy(eventHistory[0].compartment, compartment, sizeof(eventHistory[0].compartment));
  strncpy(eventHistory[0].resolution, resolution, sizeof(eventHistory[0].resolution));
  eventHistory[0].elapsedSeconds = elapsedSec;

  time_t triggerEpoch = currentAlert.alertStartEpoch;
  struct tm trTm;
  localtime_r(&triggerEpoch, &trTm);
  strftime(eventHistory[0].triggerTime, sizeof(eventHistory[0].triggerTime), "%Y-%m-%d %H:%M:%S", &trTm);

  String nowStr = getCurrentTimeString();
  strncpy(eventHistory[0].resolvedTime, nowStr.c_str(), sizeof(eventHistory[0].resolvedTime));

  eventCount++;
  Serial.printf("[EVENT LOGGED] ID: %s | %s | %s | %ds\n", eventHistory[0].eventId, compartment, resolution, elapsedSec);
}

// ── Notify Drawer 1 (ESP1) or Drawer 2 (ESP2) LEDs over Wi-Fi ────────────────
void notifyDrawerLED(int drawer, int slot, const String& action) {
  int col = slot % 7;
  // Also output to Serial for wired UART bridge (e.g. LED:1:0:alert)
  Serial.printf("LED:%d:%d:%s\n", drawer, slot, action.c_str());

  if (WiFi.status() != WL_CONNECTED) return;

  char sep = (action.indexOf('?') != -1) ? '&' : '?';
  String targetUrl = "";
  if (drawer == 1) {
    String host = (dynamicDrawer1Host.length() > 0) ? dynamicDrawer1Host : String(DRAWER1_HOST);
    targetUrl = host + "/" + action + sep + "slot=" + String(slot) + "&col=" + String(col);
  } else if (drawer == 2) {
    String host = (dynamicDrawer2Host.length() > 0) ? dynamicDrawer2Host : String(DRAWER2_HOST);
    int localSlot = (slot >= 21) ? (slot - 21) : slot;
    targetUrl = host + "/" + action + sep + "slot=" + String(localSlot) + "&col=" + String(localSlot % 7);
  }

  if (targetUrl.length() > 0) {
    HTTPClient http;
    http.begin(targetUrl);
    http.setTimeout(1200); // 1.2s timeout
    int code = http.GET();
    http.end();
    Serial.printf("[DRAWER HTTP] %s -> HTTP %d\n", targetUrl.c_str(), code);
  }
}

// ── Notify ESP1 / ESP2 when Reed Switch Opens or Closes ─────────────────────
void notifyDrawerDoorState(int drawer, bool isOpen) {
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  String host = "";
  if (drawer == 1) {
    host = (dynamicDrawer1Host.length() > 0) ? dynamicDrawer1Host : String(DRAWER1_HOST);
  } else {
    host = (dynamicDrawer2Host.length() > 0) ? dynamicDrawer2Host : String(DRAWER2_HOST);
  }
  String url = host + "/door?state=" + (isOpen ? "open" : "closed");
  http.begin(url);
  http.setTimeout(1200);
  int code = http.GET();
  http.end();
  Serial.printf("[DOOR HTTP] Drawer %d -> %s (HTTP %d)\n", drawer, isOpen ? "OPEN (LEDs ON)" : "CLOSED (LEDs OFF)", code);
}

// ── UDP Automatic Discovery Handler for ESP32 Master ──────────────────────────
void handleUDPDiscovery() {
  if (WiFi.status() != WL_CONNECTED) return;
  unsigned long now = millis();

  // 1. Periodically broadcast discovery beacon every 3 seconds to LAN
  if (now - lastDiscoveryBeacon >= 3000) {
    lastDiscoveryBeacon = now;
    udpDiscovery.beginPacket(IPAddress(255, 255, 255, 255), UDP_DISCOVERY_PORT);
    udpDiscovery.print("DISPENSER:ESP32_MASTER:80");
    udpDiscovery.endPacket();
  }

  // 2. Listen for UDP beacons from Arduino R4, ESP1, ESP2, or Mobile app
  int packetSize = udpDiscovery.parsePacket();
  if (packetSize > 0) {
    char buf[64];
    int len = udpDiscovery.read(buf, sizeof(buf) - 1);
    if (len > 0) {
      buf[len] = '\0';
      String msg = String(buf);
      msg.trim();
      IPAddress senderIP = udpDiscovery.remoteIP();

      if (msg.startsWith("DISPENSER:ARDUINO_R4")) {
        String newHost = "http://" + senderIP.toString();
        if (dynamicArduinoHost != newHost) {
          dynamicArduinoHost = newHost;
          Serial.println("[UDP] Discovered Arduino Uno R4 WiFi at: " + dynamicArduinoHost);
        }
      } else if (msg.startsWith("DISPENSER:ESP1")) {
        dynamicDrawer1Host = "http://" + senderIP.toString();
        Serial.println("[UDP] Discovered ESP1 (Drawer 1) at: " + dynamicDrawer1Host);
      } else if (msg.startsWith("DISPENSER:ESP2")) {
        dynamicDrawer2Host = "http://" + senderIP.toString();
        Serial.println("[UDP] Discovered ESP2 (Drawer 2) at: " + dynamicDrawer2Host);
      } else if (msg.startsWith("DISPENSER_DISCOVER")) {
        // Direct reply to Mobile App query
        udpDiscovery.beginPacket(senderIP, udpDiscovery.remotePort());
        udpDiscovery.print("DISPENSER:ESP32_MASTER:80");
        udpDiscovery.endPacket();
      }
    }
  }
}

// ── Notify Arduino Controller over Serial & Wi-Fi ─────────────────────────────
void notifyArduinoAlert(const String& compartment, int drawer = 1) {
  // 1. Output to Serial for debug / USB monitor
  Serial.printf("ALERT:%s\n", compartment.c_str());

  // 2. Also send HTTP command to Arduino over Wi-Fi
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    String encComp = compartment;
    encComp.replace(' ', '+');
    String host = (dynamicArduinoHost.length() > 0) ? dynamicArduinoHost : String(ARDUINO_HOST);
    String url = host + "/alert?compartment=" + encComp + "&drawer=" + String(drawer);
    http.begin(url);
    http.setTimeout(1500);
    http.GET();
    http.end();
  }
}

void notifyArduinoStop() {
  Serial.println("STOP");
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    String host = (dynamicArduinoHost.length() > 0) ? dynamicArduinoHost : String(ARDUINO_HOST);
    http.begin(host + "/stop-vibrate");
    http.setTimeout(1500);
    http.GET();
    http.end();
  }
}

// ── NVS Flash Storage ────────────────────────────────────────────────────────
void saveSchedulesToNVS() {
  preferences.begin("dispenser", false);
  preferences.putInt("count", scheduleCount);

  for (int i = 0; i < scheduleCount; i++) {
    String pfx = "s" + String(i) + "_";
    preferences.putString((pfx + "id").c_str(), schedules[i].id);
    preferences.putInt((pfx + "h").c_str(), schedules[i].hour);
    preferences.putInt((pfx + "m").c_str(), schedules[i].minute);
    preferences.putString((pfx + "d").c_str(), schedules[i].date);
    preferences.putString((pfx + "c").c_str(), schedules[i].compartment);
    preferences.putInt((pfx + "s").c_str(), schedules[i].slotIndex);
    preferences.putBool((pfx + "a").c_str(), schedules[i].active);
    preferences.putULong((pfx + "fired").c_str(), schedules[i].lastTriggeredDate);
    preferences.putString((pfx + "slots").c_str(), schedules[i].slots);
  }
  preferences.end();
  Serial.printf("[NVS] Saved %d schedules to flash memory.\n", scheduleCount);
}

void loadSchedulesFromNVS() {
  preferences.begin("dispenser", true);
  scheduleCount = preferences.getInt("count", 0);
  if (scheduleCount > MAX_SCHEDULES) scheduleCount = MAX_SCHEDULES;

  for (int i = 0; i < scheduleCount; i++) {
    String pfx = "s" + String(i) + "_";
    String id = preferences.getString((pfx + "id").c_str(), "");
    snprintf(schedules[i].id, sizeof(schedules[i].id), "%s", id.c_str());
    schedules[i].hour = preferences.getInt((pfx + "h").c_str(), 8);
    schedules[i].minute = preferences.getInt((pfx + "m").c_str(), 0);
    String d = preferences.getString((pfx + "d").c_str(), "");
    snprintf(schedules[i].date, sizeof(schedules[i].date), "%s", d.c_str());
    String c = preferences.getString((pfx + "c").c_str(), "");
    snprintf(schedules[i].compartment, sizeof(schedules[i].compartment), "%s", c.c_str());
    schedules[i].slotIndex = preferences.getInt((pfx + "s").c_str(), 0);
    schedules[i].active = preferences.getBool((pfx + "a").c_str(), true);
    schedules[i].lastTriggeredDate = preferences.getULong((pfx + "fired").c_str(), 0);
    String sl = preferences.getString((pfx + "slots").c_str(), "");
    snprintf(schedules[i].slots, sizeof(schedules[i].slots), "%s", sl.c_str());
  }
  preferences.end();
  Serial.printf("[NVS] Loaded %d schedules from flash.\n", scheduleCount);
}

int findScheduleIndex(const char* id) {
  for (int i = 0; i < scheduleCount; i++) {
    if (strcmp(schedules[i].id, id) == 0) return i;
  }
  return -1;
}

int parseCompartmentSlots(const String& compartment, const String& slotsStr, int defaultSlot, int outSlots[], int maxSlots) {
  int count = 0;
  if (slotsStr.length() > 0) {
    int start = 0;
    while (start < (int)slotsStr.length() && count < maxSlots) {
      int comma = slotsStr.indexOf(',', start);
      if (comma == -1) comma = slotsStr.length();
      String token = slotsStr.substring(start, comma);
      token.trim();
      if (token.length() > 0) {
        int s = token.toInt();
        if (s >= 0 && s < 42) {
          outSlots[count++] = s;
        }
      }
      start = comma + 1;
    }
  }

  if (count == 0) {
    String compUpper = compartment;
    compUpper.toUpperCase();
    int currentDrawer = (compUpper.indexOf("DRAWER 2") != -1 || compUpper.indexOf("2:") == 0) ? 2 : 1;

    for (unsigned int i = 0; i + 1 < compUpper.length() && count < maxSlots; i++) {
      if (compUpper.substring(i).startsWith("DRAWER 2") || compUpper.substring(i).startsWith("2:")) {
        currentDrawer = 2;
      } else if (compUpper.substring(i).startsWith("DRAWER 1") || compUpper.substring(i).startsWith("1:")) {
        currentDrawer = 1;
      }

      if (compUpper[i] >= 'A' && compUpper[i] <= 'G' && compUpper[i + 1] >= '1' && compUpper[i + 1] <= '3') {
        int slot = (currentDrawer == 2 ? 21 : 0) + (compUpper[i + 1] - '1') * 7 + (compUpper[i] - 'A');
        bool dup = false;
        for (int k = 0; k < count; k++) {
          if (outSlots[k] == slot) { dup = true; break; }
        }
        if (!dup) {
          outSlots[count++] = slot;
        }
        i++;
      }
    }
  }

  if (count == 0 && defaultSlot >= 0 && defaultSlot < 42) {
    outSlots[count++] = defaultSlot;
  }
  return count;
}

bool addOrUpdateSchedule(const char* id, int h, int m, const char* d, const char* c, int slot, const char* slotsStr = "") {
  int idx = findScheduleIndex(id);
  bool unchanged = idx >= 0 && schedules[idx].hour == h && schedules[idx].minute == m &&
                   strcmp(schedules[idx].date, d) == 0 &&
                   strcmp(schedules[idx].compartment, c) == 0 && schedules[idx].slotIndex == slot &&
                   (slotsStr == NULL || strlen(slotsStr) == 0 || strcmp(schedules[idx].slots, slotsStr) == 0);
  if (idx == -1) {
    if (scheduleCount >= MAX_SCHEDULES) return false;
    idx = scheduleCount++;
  }

  snprintf(schedules[idx].id, sizeof(schedules[idx].id), "%s", id);
  schedules[idx].hour = h;
  schedules[idx].minute = m;
  snprintf(schedules[idx].date, sizeof(schedules[idx].date), "%s", d);
  snprintf(schedules[idx].compartment, sizeof(schedules[idx].compartment), "%s", c);
  schedules[idx].slotIndex = slot;
  if (slotsStr != NULL && strlen(slotsStr) > 0) {
    snprintf(schedules[idx].slots, sizeof(schedules[idx].slots), "%s", slotsStr);
  } else if (!unchanged || strlen(schedules[idx].slots) == 0) {
    snprintf(schedules[idx].slots, sizeof(schedules[idx].slots), "%d", slot);
  }
  if (!unchanged) {
    schedules[idx].active = true;
    schedules[idx].lastTriggeredDate = 0;
  }

  saveSchedulesToNVS();
  return true;
}

bool deleteSchedule(const char* id) {
  int idx = findScheduleIndex(id);
  if (idx == -1) return false;

  for (int i = idx; i < scheduleCount - 1; i++) {
    schedules[i] = schedules[i + 1];
  }
  scheduleCount--;
  saveSchedulesToNVS();
  Serial.printf("[SCHEDULE] Deleted schedule: %s\n", id);
  return true;
}

// ── Check Alarms & Trigger 20-Min Grace Period ───────────────────────────────
void checkAlarms() {
  static unsigned long lastCheckTime = 0;
  if (millis() - lastCheckTime < 1000) return; // Check once per second
  lastCheckTime = millis();

  // If an alert is already actively ticking within its 20-min window:
  if (currentAlert.isAlerting) {
    time_t now = getCurrentEpoch();
    int elapsed = (int)(now - currentAlert.alertStartEpoch);

    // Periodic reminder vibration every 3 minutes (180s)
    if (millis() - currentAlert.lastReminderVibrateMs >= 180000UL) {
      currentAlert.lastReminderVibrateMs = millis();
      startVibrationAlert(2, 500, 250);
      Serial.println("[ALERT] Periodic 3-minute reminder vibration pulse fired.");
    }

    // 20-minute window expired (1200 seconds) -> MISSED
    if (elapsed >= GRACE_PERIOD_SECONDS) {
      Serial.println("\n⌛ [ALERT TIMEOUT] 20 minutes elapsed without intake. Dose marked MISSED.");
      logDoseEvent(currentAlert.scheduleId, currentAlert.compartment, "MISSED", GRACE_PERIOD_SECONDS);

      // Turn off alerting LEDs
      for (int s = 0; s < currentAlert.alertSlotCount; s++) {
        int slot = currentAlert.alertSlots[s];
        int drawer = (slot >= 21) ? 2 : 1;
        notifyDrawerLED(drawer, slot, "clear-alert");
      }
      if (currentAlert.alertSlotCount == 0) {
        int drawer = (currentAlert.slotIndex >= 21) ? 2 : 1;
        notifyDrawerLED(drawer, currentAlert.slotIndex, "clear-alert");
      }

      // Signal Arduino LCD
      Serial.printf("MISSED:%s\n", currentAlert.compartment);

      stopVibration();
      currentAlert.isAlerting = false;
    }
  }

  // Continue checking while alerting: a later due time replaces the old light.
  // Check if any schedule matches the current minute
  struct tm timeinfo;
  bool gotTime = getLocalTime(&timeinfo, 50);
  if (!gotTime) {
    // Offline / local Wi-Fi fallback (uses phone-calibrated RTC epoch if NTP not reachable)
    time_t now = getCurrentEpoch();
    if (now > 100000) {
      now += gmtOffset_sec;
      gmtime_r(&now, &timeinfo);
      gotTime = true;
    }
  }
  if (!gotTime) return;

  int currentHour = timeinfo.tm_hour;
  int currentMinute = timeinfo.tm_min;
  int currentDay = (timeinfo.tm_year + 1900) * 10000 +
                   (timeinfo.tm_mon + 1) * 100 + timeinfo.tm_mday;
  int currentMonth = timeinfo.tm_mon + 1;
  int currentYear = timeinfo.tm_year + 1900;

  char todayDateStr[16];
  snprintf(todayDateStr, sizeof(todayDateStr), "%04d-%02d-%02d", currentYear, currentMonth, timeinfo.tm_mday);

  bool triggeredGroup = false;
  for (int i = 0; i < scheduleCount; i++) {
    if (!schedules[i].active || schedules[i].lastTriggeredDate == (unsigned long)currentDay) continue;

    if (schedules[i].hour == currentHour && schedules[i].minute == currentMinute) {
      bool dateMatches = (strlen(schedules[i].date) == 0) || (strcmp(schedules[i].date, todayDateStr) == 0);

      if (dateMatches) {
        Serial.printf("\n🚨 [ALARM TRIGGERED!] Time: %02d:%02d | Compartment: %s | ID: %s\n",
                      schedules[i].hour, schedules[i].minute, schedules[i].compartment, schedules[i].id);

        bool extendGroup = triggeredGroup;
        triggeredGroup = true;
        lastTriggeredMinute = currentMinute;
        lastTriggeredHour = currentHour;
        lastTriggeredDay = currentDay;

        // Clear the previous indication, including when it was in the other
        // drawer. Replacement is not a pickup confirmation.
        if (currentAlert.isAlerting && !extendGroup) {
          for (int s = 0; s < currentAlert.alertSlotCount; s++) {
            int slot = currentAlert.alertSlots[s];
            int previousDrawer = slot >= 21 ? 2 : 1;
            notifyDrawerLED(previousDrawer, slot, "clear-alert");
          }
          if (currentAlert.alertSlotCount == 0) {
            int previousDrawer = currentAlert.slotIndex >= 21 ? 2 : 1;
            notifyDrawerLED(previousDrawer, currentAlert.slotIndex, "clear-alert");
          }
        }
        // Persist before notifying hardware so a reboot cannot replay this dose.
        schedules[i].lastTriggeredDate = currentDay;
        if (strlen(schedules[i].date) > 0) schedules[i].active = false;
        saveSchedulesToNVS();

        // Parse slots for this schedule (bulk or single)
        int alertSlots[MAX_BULK_SLOTS];
        int alertCount = parseCompartmentSlots(schedules[i].compartment, schedules[i].slots, schedules[i].slotIndex, alertSlots, MAX_BULK_SLOTS);

        // 1. Start a fresh 20-minute window for the newly due occurrence.
        {
          currentAlert.isAlerting = true;
          strncpy(currentAlert.scheduleId, schedules[i].id, sizeof(currentAlert.scheduleId));
          if (!extendGroup) currentAlert.compartment[0] = '\0';
          String combined = String(currentAlert.compartment);
          if (extendGroup) combined += "; ";
          combined += String(schedules[i].compartment);
          snprintf(currentAlert.compartment, sizeof(currentAlert.compartment), "%s", combined.c_str());
          currentAlert.slotIndex = alertSlots[0];
          if (!extendGroup) currentAlert.alertSlotCount = 0;
          for (int s = 0; s < alertCount; s++) {
            bool exists = false;
            for (int k = 0; k < currentAlert.alertSlotCount; k++) if (currentAlert.alertSlots[k] == alertSlots[s]) exists = true;
            if (!exists && currentAlert.alertSlotCount < MAX_BULK_SLOTS)
              currentAlert.alertSlots[currentAlert.alertSlotCount++] = alertSlots[s];
          }
          currentAlert.relayNum = (alertSlots[0] >= 21) ? 3 : 1;
          currentAlert.alertStartEpoch = getCurrentEpoch();
          currentAlert.lastReminderVibrateMs = millis();

          // 2. Start strong continuous vibration for alert (60 seconds)
          startVibrationAlert(1, 60000UL, 0);
        }

        // 3. Trigger compartment LED on ESP1 (Drawer 1) or ESP2 (Drawer 2) for each scheduled item
        bool hasDrawer1 = false;
        bool hasDrawer2 = false;
        for (int s = 0; s < alertCount; s++) {
          int slot = alertSlots[s];
          int slotDrawer = (slot >= 21) ? 2 : 1;
          if (slotDrawer == 1) hasDrawer1 = true;
          if (slotDrawer == 2) hasDrawer2 = true;
          notifyDrawerLED(slotDrawer, slot, "alert");
        }

        // 4. Trigger Hardware Relay & Arduino Buzzer/LCD for the drawers
        if (hasDrawer1) triggerDispenseRelay(1);
        if (hasDrawer2) triggerDispenseRelay(3);


        // Reset timer so blocking HTTP calls to ESP1/ESP2 & Arduino do NOT consume vibration duration!
        vibrationStateTimer = millis();

      }
    }
  }
  if (triggeredGroup) notifyArduinoAlert(currentAlert.compartment, 1);
}

// ── HTTP CORS Headers ────────────────────────────────────────────────────────
void sendCORSHeaders() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
}

// ── HTTP Handlers ────────────────────────────────────────────────────────────

void handleOptions() {
  sendCORSHeaders();
  server.send(204);
}

// GET /status (Returns full state contract matching specification)
void handleStatus() {
  sendCORSHeaders();
  String activeIP = (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : WiFi.softAPIP().toString();

  String json = "{";
  json += "\"status\":\"online\",";
  json += "\"device\":\"ESP32-Mini-Dispenser\",";
  json += "\"ip\":\"" + activeIP + "\",";
  json += "\"arduino_ip\":\"" + (dynamicArduinoHost.length() > 0 ? dynamicArduinoHost : String(ARDUINO_HOST)) + "\",";
  json += "\"drawer1_ip\":\"" + (dynamicDrawer1Host.length() > 0 ? dynamicDrawer1Host : String(DRAWER1_HOST)) + "\",";
  json += "\"drawer2_ip\":\"" + (dynamicDrawer2Host.length() > 0 ? dynamicDrawer2Host : String(DRAWER2_HOST)) + "\",";
  json += "\"device_time\":\"" + getCurrentTimeString() + "\",";
  json += "\"schedules_count\":" + String(scheduleCount) + ",";
  json += "\"is_vibrating\":" + String(isVibrating ? "true" : "false") + ",";

  // active_alert object
  json += "\"active_alert\":{";
  json += "\"is_alerting\":" + String(currentAlert.isAlerting ? "true" : "false");
  if (currentAlert.isAlerting) {
    time_t now = getCurrentEpoch();
    int elapsed = (int)(now - currentAlert.alertStartEpoch);
    int remaining = max(0, GRACE_PERIOD_SECONDS - elapsed);

    json += ",\"schedule_id\":\"" + String(currentAlert.scheduleId) + "\"";
    json += ",\"compartment\":\"" + String(currentAlert.compartment) + "\"";
    json += ",\"slot_index\":" + String(currentAlert.slotIndex);
    json += ",\"relay_num\":" + String(currentAlert.relayNum);
    json += ",\"elapsed_seconds\":" + String(elapsed);
    json += ",\"remaining_seconds\":" + String(remaining);
  }
  json += "}";

  json += "}";
  server.send(200, "application/json", json);
}

// GET /events (Returns event history buffer for offline reconciliation)
void handleEvents() {
  sendCORSHeaders();
  int limit = eventCount;
  if (server.hasArg("limit")) {
    int userLimit = server.arg("limit").toInt();
    if (userLimit > 0 && userLimit < limit) limit = userLimit;
  }

  String json = "{\"status\":\"ok\",\"events\":[";
  for (int i = 0; i < limit; i++) {
    if (i > 0) json += ",";
    json += "{";
    json += "\"event_id\":\"" + String(eventHistory[i].eventId) + "\",";
    json += "\"schedule_id\":\"" + String(eventHistory[i].scheduleId) + "\",";
    json += "\"compartment\":\"" + String(eventHistory[i].compartment) + "\",";
    json += "\"trigger_time\":\"" + String(eventHistory[i].triggerTime) + "\",";
    json += "\"resolved_time\":\"" + String(eventHistory[i].resolvedTime) + "\",";
    json += "\"resolution\":\"" + String(eventHistory[i].resolution) + "\",";
    json += "\"elapsed_seconds\":" + String(eventHistory[i].elapsedSeconds);
    json += "}";
  }
  json += "]}";
  server.send(200, "application/json", json);
}

// GET /stop-vibrate or POST /dispense-confirm (Confirm dose taken & silence alarm)
void handleStopVibrate() {
  sendCORSHeaders();
  stopVibration();

  if (currentAlert.isAlerting) {
    time_t now = getCurrentEpoch();
    int elapsed = (int)(now - currentAlert.alertStartEpoch);

    logDoseEvent(currentAlert.scheduleId, currentAlert.compartment, "TAKEN", elapsed);

    // Turn off alerting LED for all slots
    for (int s = 0; s < currentAlert.alertSlotCount; s++) {
      int slot = currentAlert.alertSlots[s];
      int drawer = (slot >= 21) ? 2 : 1;
      notifyDrawerLED(drawer, slot, "clear-alert");
    }
    if (currentAlert.alertSlotCount == 0) {
      int drawer = (currentAlert.slotIndex >= 21) ? 2 : 1;
      notifyDrawerLED(drawer, currentAlert.slotIndex, "clear-alert");
    }

    // Signal Arduino over Serial & Wi-Fi
    notifyArduinoStop();

    currentAlert.isAlerting = false;
    server.send(200, "application/json", "{\"status\":\"ok\",\"action\":\"stopped\",\"marked_status\":\"TAKEN\",\"elapsed_seconds\":" + String(elapsed) + "}");
  } else {
    server.send(200, "application/json", "{\"status\":\"ok\",\"action\":\"stopped\"}");
  }
}

// GET/POST /vibrate
void handleVibrate() {
  sendCORSHeaders();
  int duration = 60000; // Default 1 minute continuous vibration on due
  int pulses = 1;

  if (server.hasArg("duration")) duration = server.arg("duration").toInt();
  if (server.hasArg("pulses")) pulses = server.arg("pulses").toInt();

  unsigned long onMs = (pulses > 1) ? (duration / pulses) : duration;
  if (pulses > 1 && onMs > 1000) onMs = 700;
  unsigned long offMs = (pulses > 1) ? 250 : 0;

  startVibrationAlert(pulses, onMs, offMs);
  server.send(200, "application/json", "{\"status\":\"ok\",\"action\":\"vibrating\",\"pulses\":" + String(pulses) + ",\"duration\":" + String(duration) + "}");
}

int compartmentSlot(const String& compartment, int drawer) {
  String label = compartment;
  label.toUpperCase();
  for (unsigned int i = 0; i + 1 < label.length(); i++) {
    if (label[i] >= 'A' && label[i] <= 'G' && label[i + 1] >= '1' && label[i + 1] <= '3') {
      return (drawer == 2 ? 21 : 0) + (label[i + 1] - '1') * 7 + label[i] - 'A';
    }
  }
  return -1;
}

// GET/POST /alert (Triggered by mobile watchdog or Arduino over Wi-Fi)
void handleAlert() {
  sendCORSHeaders();
  String comp = server.hasArg("compartment") ? server.arg("compartment") : "Drawer 1 (A1)";
  int drw = 1;
  if (server.hasArg("drawer")) drw = server.arg("drawer").toInt();
  else if (comp.indexOf("Drawer 2") != -1 || comp.indexOf("2:") == 0) drw = 2;

  int slot = compartmentSlot(comp, drw);
  if (server.hasArg("slotIndex")) slot = server.arg("slotIndex").toInt();
  if (drw == 2 && slot >= 0 && slot < 21) slot += 21;
  if (slot < 0 || slot >= 42) {
    server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Invalid compartment or slotIndex\"}");
    return;
  }
  drw = slot >= 21 ? 2 : 1;
  String upperComp = comp;
  upperComp.toUpperCase();
  if (upperComp.indexOf("DRAWER") == -1) comp = "Drawer " + String(drw) + " (" + comp + ")";
  int slots[MAX_BULK_SLOTS];
  int count = parseCompartmentSlots(comp, "", slot, slots, MAX_BULK_SLOTS);
  bool duplicate = currentAlert.isAlerting && currentAlert.alertSlotCount == count &&
                   comp == String(currentAlert.compartment);
  for (int i = 0; i < count && duplicate; i++) duplicate = currentAlert.alertSlots[i] == slots[i];
  if (!duplicate) {
    if (currentAlert.isAlerting) {
      for (int i = 0; i < currentAlert.alertSlotCount; i++) {
        int previous = currentAlert.alertSlots[i];
        notifyDrawerLED(previous >= 21 ? 2 : 1, previous, "clear-alert");
      }
    }
    currentAlert.isAlerting = true;
    strncpy(currentAlert.scheduleId, "remote_alert", sizeof(currentAlert.scheduleId));
    snprintf(currentAlert.compartment, sizeof(currentAlert.compartment), "%s", comp.c_str());
    currentAlert.slotIndex = slots[0];
    currentAlert.alertSlotCount = count;
    for (int i = 0; i < count; i++) {
      currentAlert.alertSlots[i] = slots[i];
      notifyDrawerLED(slots[i] >= 21 ? 2 : 1, slots[i], "alert");
    }
    currentAlert.relayNum = (drw == 2) ? 3 : 1;
    currentAlert.alertStartEpoch = getCurrentEpoch();
    currentAlert.lastReminderVibrateMs = millis();
  }

  // Start continuous 1-minute vibration alert
  startVibrationAlert(1, 60000UL, 0);
  Serial.printf("[ALERT] Hardware alert received for %s (Drawer %d) -> Vibration active (60s)\n", comp.c_str(), drw);
  server.send(200, "application/json", "{\"status\":\"ok\",\"action\":\"alert_started\",\"drawer\":" + String(drw) + ",\"vibrating\":true}");
}

// GET/POST /solenoid?drawer=1&state=unlock (or lock)
void handleSolenoid() {
  sendCORSHeaders();
  int drw = 0;
  if (server.hasArg("drawer")) drw = server.arg("drawer").toInt();
  String state = server.hasArg("state") ? server.arg("state") : "unlock";
  bool unlock = (state.equalsIgnoreCase("unlock") || state.equalsIgnoreCase("open") || state == "1");

  if (unlock) {
    if (drw == 1 || drw == 0) {
      if (RELAY1_PIN >= 0) digitalWrite(RELAY1_PIN, RELAY_ON);
      if (RELAY2_PIN >= 0) digitalWrite(RELAY2_PIN, RELAY_ON);
    }
    if (drw == 2 || drw == 0) {
      if (RELAY3_PIN >= 0) digitalWrite(RELAY3_PIN, RELAY_ON);
      if (RELAY4_PIN >= 0) digitalWrite(RELAY4_PIN, RELAY_ON);
    }
    Serial.printf("SOLENOID:%d UNLOCK\n", drw);
    Serial.printf("[ESP32] Solenoid Drawer %d UNLOCKED\n", drw);
  } else {
    if (drw == 1 || drw == 0) {
      if (RELAY1_PIN >= 0) digitalWrite(RELAY1_PIN, RELAY_OFF);
      if (RELAY2_PIN >= 0) digitalWrite(RELAY2_PIN, RELAY_OFF);
    }
    if (drw == 2 || drw == 0) {
      if (RELAY3_PIN >= 0) digitalWrite(RELAY3_PIN, RELAY_OFF);
      if (RELAY4_PIN >= 0) digitalWrite(RELAY4_PIN, RELAY_OFF);
    }
    Serial.printf("SOLENOID:%d LOCK\n", drw);
    Serial.printf("[ESP32] Solenoid Drawer %d LOCKED\n", drw);
  }

  server.send(200, "application/json", "{\"status\":\"ok\",\"action\":\"solenoid\",\"drawer\":" + String(drw) + ",\"unlocked\":" + String(unlock ? "true" : "false") + "}");
}

// GET /dispense
void handleDispense() {
  sendCORSHeaders();
  int relayNum = 1;
  if (server.hasArg("relay")) relayNum = server.arg("relay").toInt();

  startVibrationAlert(2, 500, 200);
  triggerDispenseRelay(relayNum);
  server.send(200, "application/json", "{\"status\":\"ok\",\"dispensed_relay\":" + String(relayNum) + ",\"vibrating\":true}");
}

// GET/POST /set-schedule (Includes client_epoch calibration)
void handleSetSchedule() {
  sendCORSHeaders();

  String id = server.hasArg("id") ? server.arg("id") : String(millis());
  int hour = server.hasArg("hour") ? server.arg("hour").toInt() : 8;
  int minute = server.hasArg("minute") ? server.arg("minute").toInt() : 0;
  String amPm = server.hasArg("amPm") ? server.arg("amPm") : "";
  String date = server.hasArg("date") ? server.arg("date") : "";
  String compartment = server.hasArg("compartment") ? server.arg("compartment") : "";
  int slotIndex = server.hasArg("slotIndex") ? server.arg("slotIndex").toInt() : 0;

  if (amPm.equalsIgnoreCase("AM")) {
    if (hour == 12) hour = 0;
  } else if (amPm.equalsIgnoreCase("PM")) {
    if (hour != 12) hour += 12;
  }

  // Check client epoch calibration (drift correction)
  bool rtcCalibrated = false;
  if (server.hasArg("client_epoch")) {
    time_t clientEpoch = (time_t)server.arg("client_epoch").toInt();
    time_t current = getCurrentEpoch();
    if (abs((long)(clientEpoch - current)) > 5) {
      struct timeval tv = { .tv_sec = clientEpoch, .tv_usec = 0 };
      settimeofday(&tv, NULL);
      rtcCalibrated = true;
      Serial.printf("[RTC CALIBRATION] Synchronized drift (%ld sec)\n", (long)(clientEpoch - current));
    }
  }

  bool success = addOrUpdateSchedule(id.c_str(), hour, minute, date.c_str(), compartment.c_str(), slotIndex);

  if (success) {
    // Save schedule metadata; the drawer stays dark until /alert arrives.
    int drawer = (slotIndex >= 21) ? 2 : 1;
    notifyDrawerLED(drawer, slotIndex, "led?state=active");
    String resp = "{\"status\":\"ok\",\"message\":\"Schedule saved\",\"id\":\"" + id + "\",\"rtc_calibrated\":" + String(rtcCalibrated ? "true" : "false") + "}";
    server.send(200, "application/json", resp);
  } else {
    server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Failed to save schedule\"}");
  }
}

// GET/POST /set-bulk-schedule (or /bulk-schedule)
// Schedules multiple compartments (e.g. A1, A2, A3) as one schedule entity
void handleSetBulkSchedule() {
  sendCORSHeaders();

  String id = server.hasArg("id") ? server.arg("id") : String(millis());
  int hour = server.hasArg("hour") ? server.arg("hour").toInt() : 8;
  int minute = server.hasArg("minute") ? server.arg("minute").toInt() : 0;
  String amPm = server.hasArg("amPm") ? server.arg("amPm") : "";
  String date = server.hasArg("date") ? server.arg("date") : "";

  String compartments = "";
  if (server.hasArg("compartments")) compartments = server.arg("compartments");
  else if (server.hasArg("compartment")) compartments = server.arg("compartment");

  String slotsStr = server.hasArg("slots") ? server.arg("slots") : "";
  int defaultDrawer = 1;
  if (server.hasArg("drawer")) defaultDrawer = server.arg("drawer").toInt();

  // If POST JSON body is provided
  if (server.hasArg("plain")) {
    String body = server.arg("plain");
    int idIdx = body.indexOf("\"id\"");
    if (idIdx != -1) {
      int q1 = body.indexOf('"', idIdx + 4);
      int q2 = body.indexOf('"', q1 + 1);
      int q3 = body.indexOf('"', q2 + 1);
      if (q2 != -1 && q3 != -1) id = body.substring(q2 + 1, q3);
    }
    int hIdx = body.indexOf("\"hour\"");
    if (hIdx != -1) {
      int col = body.indexOf(':', hIdx);
      if (col != -1) hour = body.substring(col + 1).toInt();
    }
    int mIdx = body.indexOf("\"minute\"");
    if (mIdx != -1) {
      int col = body.indexOf(':', mIdx);
      if (col != -1) minute = body.substring(col + 1).toInt();
    }
    int dIdx = body.indexOf("\"date\"");
    if (dIdx != -1) {
      int q1 = body.indexOf('"', dIdx + 6);
      int q2 = body.indexOf('"', q1 + 1);
      int q3 = body.indexOf('"', q2 + 1);
      if (q2 != -1 && q3 != -1) date = body.substring(q2 + 1, q3);
    }
    int cIdx = body.indexOf("\"compartments\"");
    if (cIdx != -1) {
      int q1 = body.indexOf('"', cIdx + 14);
      int q2 = body.indexOf('"', q1 + 1);
      int q3 = body.indexOf('"', q2 + 1);
      if (q2 != -1 && q3 != -1) compartments = body.substring(q2 + 1, q3);
    }
    int sIdx = body.indexOf("\"slots\"");
    if (sIdx != -1) {
      int q1 = body.indexOf('"', sIdx + 7);
      int q2 = body.indexOf('"', q1 + 1);
      int q3 = body.indexOf('"', q2 + 1);
      if (q2 != -1 && q3 != -1) slotsStr = body.substring(q2 + 1, q3);
    }
  }

  if (amPm.equalsIgnoreCase("AM")) {
    if (hour == 12) hour = 0;
  } else if (amPm.equalsIgnoreCase("PM")) {
    if (hour != 12) hour += 12;
  }

  bool rtcCalibrated = false;
  if (server.hasArg("client_epoch")) {
    time_t clientEpoch = (time_t)server.arg("client_epoch").toInt();
    time_t current = getCurrentEpoch();
    if (abs((long)(clientEpoch - current)) > 5) {
      struct timeval tv = { .tv_sec = clientEpoch, .tv_usec = 0 };
      settimeofday(&tv, NULL);
      rtcCalibrated = true;
      Serial.printf("[RTC CALIBRATION] Synchronized drift (%ld sec)\n", (long)(clientEpoch - current));
    }
  }

  String canonicalComp = compartments;
  canonicalComp.trim();
  String upperComp = canonicalComp;
  upperComp.toUpperCase();
  if (upperComp.indexOf("DRAWER") == -1 && canonicalComp.length() > 0) {
    canonicalComp = "Drawer " + String(defaultDrawer) + " (" + canonicalComp + ")";
  }
  int parsedSlots[MAX_BULK_SLOTS];
  int count = parseCompartmentSlots(canonicalComp, slotsStr, -1, parsedSlots, MAX_BULK_SLOTS);
  if (count == 0 || hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
      canonicalComp.length() >= sizeof(schedules[0].compartment)) {
    server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Invalid bulk schedule\"}");
    return;
  }

  String normSlotsStr = "";
  for (int s = 0; s < count; s++) {
    if (s > 0) normSlotsStr += ",";
    normSlotsStr += String(parsedSlots[s]);
  }

  int primarySlot = count > 0 ? parsedSlots[0] : 0;
  bool success = addOrUpdateSchedule(id.c_str(), hour, minute, date.c_str(), canonicalComp.c_str(), primarySlot, normSlotsStr.c_str());

  if (success) {
    // Persisting the schedule is sufficient. Per-slot HTTP calls here delay
    // the response beyond the app timeout; LEDs are sent when the dose is due.
    String resp = "{\"status\":\"ok\",\"action\":\"bulk_schedule_saved\",\"id\":\"" + id +
                  "\",\"compartment\":\"" + canonicalComp +
                  "\",\"slots\":\"" + normSlotsStr +
                  "\",\"slot_count\":" + String(count) +
                  ",\"rtc_calibrated\":" + String(rtcCalibrated ? "true" : "false") + "}";
    server.send(200, "application/json", resp);
  } else {
    server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Failed to save bulk schedule\"}");
  }
}

// GET /delete-schedule
void handleDeleteSchedule() {
  sendCORSHeaders();
  if (!server.hasArg("id")) {
    server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Missing schedule id\"}");
    return;
  }

  String id = server.arg("id");
  int idx = findScheduleIndex(id.c_str());
  if (idx != -1) {
    int parsedSlots[MAX_BULK_SLOTS];
    int count = parseCompartmentSlots(schedules[idx].compartment, schedules[idx].slots, schedules[idx].slotIndex, parsedSlots, MAX_BULK_SLOTS);
    for (int s = 0; s < count; s++) {
      int slot = parsedSlots[s];
      int drawer = (slot >= 21) ? 2 : 1;
      notifyDrawerLED(drawer, slot, "led?state=off");
    }
  }

  bool success = deleteSchedule(id.c_str());
  if (success) {
    server.send(200, "application/json", "{\"status\":\"ok\",\"message\":\"Schedule deleted\"}");
  } else {
    server.send(404, "application/json", "{\"status\":\"error\",\"message\":\"Schedule not found\"}");
  }
}

// GET /schedules
void handleGetSchedules() {
  sendCORSHeaders();
  String json = "[";
  for (int i = 0; i < scheduleCount; i++) {
    if (i > 0) json += ",";
    json += "{";
    json += "\"id\":\"" + String(schedules[i].id) + "\",";
    json += "\"hour\":" + String(schedules[i].hour) + ",";
    json += "\"minute\":" + String(schedules[i].minute) + ",";
    json += "\"date\":\"" + String(schedules[i].date) + "\",";
    json += "\"compartment\":\"" + String(schedules[i].compartment) + "\",";
    json += "\"slotIndex\":" + String(schedules[i].slotIndex) + ",";
    json += "\"active\":" + String(schedules[i].active ? "true" : "false");
    if (strlen(schedules[i].slots) > 0) {
      json += ",\"slots\":\"" + String(schedules[i].slots) + "\"";
    }
    json += "}";
  }
  json += "]";
  server.send(200, "application/json", json);
}

// GET /test-alert?slot=0 or ?col=0 (Triggers VIBRATION + ESP1 Drawer Column Blinking)
void handleTestAlert() {
  sendCORSHeaders();
  int slot = server.hasArg("slot") ? server.arg("slot").toInt() : 0;
  if (server.hasArg("col")) {
    slot = server.arg("col").toInt(); // Col 0..6
  }
  int col = slot % 7;

  currentAlert.isAlerting = true;
  strncpy(currentAlert.scheduleId, "test_schedule", sizeof(currentAlert.scheduleId));
  snprintf(currentAlert.compartment, sizeof(currentAlert.compartment), "Col %c (Slot %d)", 'A' + col, slot);
  currentAlert.slotIndex = slot;
  currentAlert.relayNum = (slot % 4) + 1;
  currentAlert.alertStartEpoch = getCurrentEpoch();
  currentAlert.lastReminderVibrateMs = millis();

  // 1. Start strong vibration pattern on ESP32-Mini GPIO 4
  startVibrationAlert(3, 700, 300);

  // 2. Notify ESP1 (Drawer 1) over Wi-Fi to start blinking that column
  notifyDrawerLED(1, slot, "alert");

  String resp = "{\"status\":\"ok\",\"action\":\"alert_started\",\"slot\":" + String(slot) + ",\"column\":" + String(col) + ",\"vibrating\":true}";
  server.send(200, "application/json", resp);
}

// GET /dashboard
void handleRoot() {
  sendCORSHeaders();
  String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1.0'>";
  html += "<title>Smart Medicine Dispenser</title>";
  html += "<style>body{font-family:sans-serif;background:#F5F0E8;padding:20px;color:#212121;}";
  html += ".card{background:#fff;border-radius:16px;padding:20px;margin-bottom:16px;box-shadow:0 2px 8px rgba(0,0,0,0.08);}";
  html += ".btn{display:inline-block;padding:12px 20px;border-radius:10px;text-decoration:none;font-weight:700;margin-right:8px;margin-bottom:8px;}";
  html += ".btn-green{background:#2E9E4B;color:#fff;} .btn-red{background:#EF4444;color:#fff;} .btn-orange{background:#F59E0B;color:#fff;} .btn-blue{background:#2563EB;color:#fff;}";
  html += "</style></head><body>";
  html += "<h1>💊 Smart Medicine Dispenser Controller</h1>";
  html += "<div class='card'><h2>Current Status</h2>";
  html += "<p><strong>Device Time:</strong> " + getCurrentTimeString() + "</p>";
  html += "<p><strong>Active Alert:</strong> " + String(currentAlert.isAlerting ? "🚨 DUE NOW (" + String(currentAlert.compartment) + ")" : "✅ Normal") + "</p>";
  html += "<p><strong>Vibration Motor (GPIO 4):</strong> " + String(isVibrating ? "📳 VIBRATING" : "Idle") + "</p>";
  html += "<hr style='margin:16px 0;border:0;border-top:1px solid #eee;'>";
  html += "<h3>🚀 Direct Test Controls:</h3>";
  html += "<a href='/test-alert?slot=0' class='btn btn-orange'>🚨 Test Alert (Vibrate + Blink Col A)</a>";
  html += "<a href='/stop-vibrate' class='btn btn-green'>⏹ Silence / Dose Taken</a>";
  html += "<a href='/vibrate?duration=2500' class='btn btn-blue'>📳 Vibrate Motor Only</a>";
  html += "<a href='/set-schedule?slotIndex=0&hour=8&minute=0&compartment=Drawer+1+(A1)' class='btn btn-green'>📅 Schedule Col A (Slot 0)</a>";
  html += "</div></body></html>";
  server.send(200, "text/html", html);
}

// Event callbacks run on the Wi-Fi task: only log here; manage services in loop().
void logWiFiDisconnect(WiFiEvent_t event, WiFiEventInfo_t info) {
  Serial.printf("[WIFI] Disconnected: reason=%u (ESP-IDF reason code)\n",
                unsigned(info.wifi_sta_disconnected.reason));
}

bool wifiServicesReady = false;
unsigned long lastWiFiAttempt = 0;
const unsigned long WIFI_RETRY_MS = 30000;

void maintainWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    if (!wifiServicesReady) {
      String ip = WiFi.localIP().toString();
      Serial.printf("[WIFI] Connected! IP Address: %s\n", ip.c_str());
      Serial.printf("IP:%s\n", ip.c_str());
      if (MDNS.begin("dispenser")) {
        Serial.println("[MDNS] Started: http://dispenser.local");
      }
      udpDiscovery.begin(UDP_DISCOVERY_PORT);
      configTime(gmtOffset_sec, daylightOffset_sec, ntpServer1, ntpServer2);
      wifiServicesReady = true;
    }
    return;
  }

  if (wifiServicesReady) {
    udpDiscovery.stop();
    MDNS.end();
    // A hotspot can assign different addresses after restarting.
    dynamicArduinoHost = "";
    dynamicDrawer1Host = "";
    dynamicDrawer2Host = "";
    wifiServicesReady = false;
    lastWiFiAttempt = millis();
  }
  if (millis() - lastWiFiAttempt >= WIFI_RETRY_MS) {
    lastWiFiAttempt = millis();
    Serial.printf("[WIFI] Retrying '%s'; status=%d\n", ssid, int(WiFi.status()));
    WiFi.reconnect();
  }
}

// ── Setup ────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  unsigned long serialWait = millis();
  while (!Serial && (millis() - serialWait < 2500)) {
    delay(50);
  }
  delay(500);
  Serial.println("\n========================================================");
  Serial.println("  SMART MEDICINE DISPENSER - ESP32 MASTER CONTROLLER");
  Serial.println("========================================================");

  // Setup Vibration Motor Pin
  pinMode(VIBRATION_PIN, OUTPUT);
  digitalWrite(VIBRATION_PIN, VIBRATION_INACTIVE);

  // Setup Status LED Pin
  if (STATUS_LED_PIN >= 0) {
    pinMode(STATUS_LED_PIN, OUTPUT);
    digitalWrite(STATUS_LED_PIN, LED_INACTIVE);
  }

  // Setup Relay Pins
  if (RELAY1_PIN >= 0) { pinMode(RELAY1_PIN, OUTPUT); digitalWrite(RELAY1_PIN, RELAY_OFF); }
  if (RELAY2_PIN >= 0) { pinMode(RELAY2_PIN, OUTPUT); digitalWrite(RELAY2_PIN, RELAY_OFF); }
  if (RELAY3_PIN >= 0) { pinMode(RELAY3_PIN, OUTPUT); digitalWrite(RELAY3_PIN, RELAY_OFF); }
  if (RELAY4_PIN >= 0) { pinMode(RELAY4_PIN, OUTPUT); digitalWrite(RELAY4_PIN, RELAY_OFF); }

  loadSchedulesFromNVS();

  // Quick initial vibration self-test (100ms)
  startVibrationAlert(1, 100, 50);

  // Start Wi-Fi in pure Station Mode (Direct connection to router, No Hotspot)
  WiFi.onEvent(logWiFiDisconnect, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);
  lastWiFiAttempt = millis();
  WiFi.begin(ssid, password);
  Serial.printf("\n[WIFI] Connecting directly to '%s'...\n", ssid);

  int retries = 0;
  while (WiFi.status() != WL_CONNECTED && retries < 40) {
    delay(500);
    Serial.print(".");
    retries++;
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("\n[WIFI] Not connected to '%s'; status=%d. Retrying every 30s.\n",
                  ssid, int(WiFi.status()));
  }

  // Setup Web Server Endpoints
  server.on("/", HTTP_GET, handleRoot);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/ping", HTTP_GET, handleStatus);
  server.on("/events", HTTP_GET, handleEvents);
  server.on("/alert", HTTP_GET, handleAlert);
  server.on("/alert", HTTP_POST, handleAlert);
  server.on("/vibrate", HTTP_GET, handleVibrate);
  server.on("/vibrate", HTTP_POST, handleVibrate);
  server.on("/stop-vibrate", HTTP_GET, handleStopVibrate);
  server.on("/dispense-confirm", HTTP_POST, handleStopVibrate);
  server.on("/dispense", HTTP_GET, handleDispense);
  server.on("/solenoid", HTTP_GET, handleSolenoid);
  server.on("/solenoid", HTTP_POST, handleSolenoid);
  server.on("/set-schedule", HTTP_GET, handleSetSchedule);
  server.on("/set-schedule", HTTP_POST, handleSetSchedule);
  server.on("/set-bulk-schedule", HTTP_GET, handleSetBulkSchedule);
  server.on("/set-bulk-schedule", HTTP_POST, handleSetBulkSchedule);
  server.on("/bulk-schedule", HTTP_GET, handleSetBulkSchedule);
  server.on("/bulk-schedule", HTTP_POST, handleSetBulkSchedule);
  server.on("/delete-schedule", HTTP_GET, handleDeleteSchedule);
  server.on("/schedules", HTTP_GET, handleGetSchedules);
  server.on("/test-alert", HTTP_GET, handleTestAlert);

  server.on("/alert", HTTP_OPTIONS, handleOptions);
  server.on("/solenoid", HTTP_OPTIONS, handleOptions);
  server.on("/vibrate", HTTP_OPTIONS, handleOptions);
  server.on("/stop-vibrate", HTTP_OPTIONS, handleOptions);
  server.on("/set-schedule", HTTP_OPTIONS, handleOptions);
  server.on("/set-bulk-schedule", HTTP_OPTIONS, handleOptions);
  server.on("/bulk-schedule", HTTP_OPTIONS, handleOptions);

  server.begin();
  maintainWiFi();
  Serial.println("[HTTP] Server configured on port 80; reachable once Wi-Fi connects.");
}

// ── Handle Serial Messages from Arduino or Serial Monitor ───────────────────
void handleSerialFromArduino() {
  if (Serial.available() > 0) {
    String msg = Serial.readStringUntil('\n');
    msg.trim();
    if (msg.startsWith("TAKEN:")) {
      String comp = msg.substring(6);
      Serial.printf("[SERIAL <- ARDUINO] Magnetic Reed Switch Triggered! Dose TAKEN for: %s\n", comp.c_str());
      // A delayed pickup for an older compartment must not clear the new dose.
      if (currentAlert.isAlerting && comp == String(currentAlert.compartment)) handleStopVibrate();
    } else if (msg.startsWith("LED:")) {
      // Forward direct Arduino schedule/clear events to the correct drawer.
      int secondColon = msg.indexOf(':', 4);
      int thirdColon = msg.indexOf(':', secondColon + 1);
      if (secondColon > 4 && thirdColon > secondColon) {
        int drawer = msg.substring(4, secondColon).toInt();
        int slot = msg.substring(secondColon + 1, thirdColon).toInt();
        String action = msg.substring(thirdColon + 1);
        if ((drawer == 1 || drawer == 2) && slot >= 0 && slot < 21 &&
            (action == "alert" || action == "clear-alert")) {
          notifyDrawerLED(drawer, slot, action);
        }
      }
    } else if (msg.startsWith("DRAWER:1:OPEN")) {
      Serial.println("[SERIAL <- ARDUINO] Drawer 1 OPENED -> Telling ESP1 to enable LEDs!");
      notifyDrawerDoorState(1, true);
    } else if (msg.startsWith("DRAWER:1:CLOSE")) {
      Serial.println("[SERIAL <- ARDUINO] Drawer 1 CLOSED -> Telling ESP1 to turn OFF all LEDs!");
      notifyDrawerDoorState(1, false);
    } else if (msg.startsWith("DRAWER:2:OPEN")) {
      Serial.println("[SERIAL <- ARDUINO] Drawer 2 OPENED -> Telling ESP2 to enable LEDs!");
      notifyDrawerDoorState(2, true);
    } else if (msg.startsWith("DRAWER:2:CLOSE")) {
      Serial.println("[SERIAL <- ARDUINO] Drawer 2 CLOSED -> Telling ESP2 to turn OFF all LEDs!");
      notifyDrawerDoorState(2, false);
    } else if (msg.startsWith("MISSED:")) {
      Serial.println("[SERIAL <- ARDUINO] 20-Minute Grace Period Expired: MISSED");
    } else if (msg.equalsIgnoreCase("IP") || msg.equalsIgnoreCase("STATUS") || msg.equalsIgnoreCase("WIFI")) {
      Serial.println("\n============== ESP32-MINI STATUS ==============");
      if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("  Wi-Fi Network : %s\n", ssid);
        Serial.printf("  IP Address    : %s\n", WiFi.localIP().toString().c_str());
        Serial.printf("  Gateway URL   : http://%s/\n", WiFi.localIP().toString().c_str());
        Serial.println("  mDNS Address  : http://dispenser.local/");
      } else {
        Serial.printf("  Wi-Fi Status  : DISCONNECTED (Connecting to '%s'...)\n", ssid);
      }
      Serial.printf("  Vibration Pin : GPIO %d (%s)\n", VIBRATION_PIN, isVibrating ? "VIBRATING" : "IDLE");
      Serial.printf("  Schedules     : %d active in memory\n", scheduleCount);
      Serial.println("================================================\n");
    } else if (msg.equalsIgnoreCase("VIBRATE")) {
      Serial.println("[TEST] Triggering 3 vibration pulses on GPIO 4...");
      startVibrationAlert(3, 600, 300);
    } else if (msg.equalsIgnoreCase("STOP")) {
      Serial.println("[TEST] Stopping vibration.");
      stopVibration();
    }
  }
}

// ── Loop ─────────────────────────────────────────────────────────────────────
void loop() {
  maintainWiFi();
  server.handleClient();
  handleUDPDiscovery();
  updateVibrationState();
  updateDispenserState();
  checkAlarms();
  handleSerialFromArduino();

  // Periodic heartbeat every 5 seconds so IP is always visible in Serial Monitor
  static unsigned long lastHeartbeat = 0;
  if (millis() - lastHeartbeat >= 5000) {
    lastHeartbeat = millis();
    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("[ESP32-MINI] 🟢 IP: %s | http://%s | Vibration: GPIO %d\n",
                    WiFi.localIP().toString().c_str(), WiFi.localIP().toString().c_str(), VIBRATION_PIN);
    } else {
      Serial.printf("[ESP32-MINI] 🟡 Connecting to Wi-Fi '%s'...\n", ssid);
    }
  }
}
