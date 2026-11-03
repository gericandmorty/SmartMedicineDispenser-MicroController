#!/usr/bin/env python3
"""Compile real firmware logic with host shims; requires Python 3 and g++."""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
MOCK = ROOT / 'tests/arduino_mock.h'


def function(source, name):
    # Firmware functions finish with a closing brace at column zero.
    match = re.search(r'^\w[\w :*&<>]*\b' + name + r'\([^\n]*\) \{\n.*?^}', source, re.M | re.S)
    if not match:
        raise ValueError(name)
    return match.group()


def run(name, code, temp):
    source = temp / (name + '.cpp')
    binary = temp / name
    source.write_text(f'#include "{MOCK}"\n' + code)
    subprocess.run(['g++', '-std=c++17', '-I', str(temp), str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
    print(name + ': PASS', flush=True)


with tempfile.TemporaryDirectory(prefix='dispenser-tests-') as work:
    temp = Path(work)
    for header in ('Wire.h', 'LiquidCrystal_I2C.h'):
        (temp / header).write_text('// supplied by arduino_mock.h\n')
    run('arduino', f'#include "{ROOT / "arduino/arduino.ino"}"\n' + r'''
void setTime(unsigned long day, int hour, int minute) {
  clientBaseEpoch = day * 86400UL + hour * 3600UL + minute * 60UL - 28800UL;
  clientBaseMillis = fakeMillis;
}
int main() {
  assert(BUZZER_DURATION_MS == 1200000UL);
  unsigned long today, tomorrow, unused;
  assert(parseScheduleDay("2026-10-30", today));
  assert(parseScheduleDay("2026-10-31", tomorrow) && tomorrow == today + 1);
  assert(parseScheduleDay("2028-02-29", unused));
  assert(!parseScheduleDay("2026-02-29", unused));
  assert(!parseScheduleDay("2026-11-31", unused));
  assert(!parseScheduleDay("garbage", unused));
  assert(parseScheduleDay("", unused) && unused == 0);
  assert(extractShortLabel("Drawer 2 (G3)") == "G3");
  scheduleCount = 3;
  schedules[0] = {11,45,"Drawer 1 (A1)",1,0,0,true,"daily"};
  schedules[1] = {11,46,"Drawer 2 (B1)",2,today,0,true,"once"};
  schedules[2] = {11,47,"Drawer 1 (C1)",1,tomorrow,0,true,"future"};
  setTime(today, 11, 44); checkArduinoSchedules(); assert(!isAlertActive);
  setTime(today, 11, 45); checkArduinoSchedules();
  assert(alertCount == 1 && activeAlerts[0].shortLabel == "A1");
  auto start = alertStartTime;
  fakeMillis += 1000; checkArduinoSchedules(); assert(alertStartTime == start);
  setTime(today, 11, 46); checkArduinoSchedules();
  assert(alertCount == 1 && activeAlerts[0].shortLabel == "B1");
  assert(activeAlerts[0].drawer == 2 && !schedules[1].active);
  assert(pinState[SOLENOID_DRAWER1_PIN] == SOLENOID_OFF);
  resolveDoseTaken(1); assert(isAlertActive); // Wrong drawer cannot confirm.
  resolveDoseTaken(2); assert(!isAlertActive);
  checkArduinoSchedules(); assert(!isAlertActive); // No relight after pickup.
  setTime(today, 11, 47); checkArduinoSchedules(); assert(!isAlertActive);
  setTime(tomorrow, 11, 45); checkArduinoSchedules(); assert(isAlertActive);
  assert(activeAlerts[0].shortLabel == "A1");
  resolveDoseTaken(1);
  setTime(tomorrow, 11, 46); checkArduinoSchedules(); assert(!isAlertActive);
  setTime(tomorrow, 11, 47); checkArduinoSchedules();
  assert(isAlertActive && activeAlerts[0].shortLabel == "C1");
  // Buzzer remains eligible after the former 2-minute cutoff, expires at 20.
  pinState[REED_SWITCH_DRAWER1] = LOW; pinState[REED_SWITCH_DRAWER2] = LOW;
  fakeMillis = alertStartTime + 121000UL;
  loop(); fakeMillis += 1000; loop();
  assert(isAlertActive && pinState[BUZZER_RELAY_PIN] == RELAY_ON);
  fakeMillis = alertStartTime + BUZZER_DURATION_MS; loop();
  assert(!isAlertActive && pinState[BUZZER_RELAY_PIN] == RELAY_OFF);
  // A retry within the picked-up minute must not reopen the same dose.
  fakeMillis += 1000; setTime(tomorrow + 1, 11, 45); checkArduinoSchedules();
  resolveDoseTaken(1); startAlert("Drawer 1 (A1)", 1); assert(!isAlertActive);
}
''', temp)

    for drawer in ('esp1', 'esp2'):
        source = (ROOT / drawer / (drawer + '.ino')).read_text()
        declarations = source[source.index('const int ledPins'):source.index('WebServer server')]
        names = ('parseSlotIndex', 'setSlotLED', 'allOff', 'setScheduleSlot',
                 'setAlertSlot', 'clearAll', 'updateLEDState', 'handleDoorState',
                 'handleClearAlert', 'handleSerialCommands')
        code = declarations + '\nServerMock server;\nvoid sendCORSHeaders() {}\nvoid runStartupAnimation() {}\n'
        # Status-only dependencies in the serial handler.
        code += 'const char* ssid = ""; struct { String toString() { return ""; } } fakeIP;\n'
        code += 'struct { decltype(fakeIP) localIP() { return fakeIP; } int RSSI() { return 0; } } WiFi;\n'
        code += '\n'.join(function(source, name) for name in names)
        code += r'''
void assertOnly(int slot) {
  for (int i = 0; i < 21; i++) assert(pinState[ledPins[i/7][i%7]] == (i == slot ? HIGH : LOW));
}
int main() {
  isDrawerOpen = true;
  setScheduleSlot(0); setScheduleSlot(1); setScheduleSlot(2);
  updateLEDState(); assertOnly(-1); // Future schedules do not light.
  for (int slot = 0; slot < 3; slot++) {
    fakeMillis += 60000; setAlertSlot(slot); updateLEDState(); assertOnly(slot);
    auto start = alertStartedMs; fakeMillis += 100;
    setAlertSlot(slot); assert(alertStartedMs == start); // Retry is idempotent.
  }
  setAlertSlot(2, false); updateLEDState(); assertOnly(-1);
  setScheduleSlot(2); updateLEDState(); assertOnly(-1); // App sync cannot relight.
  fakeMillis += 60000; setAlertSlot(0); updateLEDState(); assertOnly(0);
  server.args["state"] = "closed"; handleDoorState(); assertOnly(-1);
  server.args["state"] = "open"; handleDoorState(); updateLEDState(); assertOnly(-1);
  fakeMillis += 86400000UL; setAlertSlot(0); updateLEDState(); assertOnly(0);
  fakeMillis += ALERT_DURATION_MS; updateLEDState(); assertOnly(-1);
  isDrawerOpen = false; setAlertSlot(1); updateLEDState(); assertOnly(-1);
  isDrawerOpen = true; updateLEDState(); assertOnly(1);
  server.args.clear(); handleClearAlert(); assertOnly(-1);
  Serial.input = "LED:OTHER:0:alert"; handleSerialCommands(); updateLEDState(); assertOnly(-1);
  Serial.input = "ALERT:Drawer OTHER (A1)"; handleSerialCommands(); updateLEDState(); assertOnly(-1);
}
'''.replace('OTHER', '2' if drawer == 'esp1' else '1')
        run(drawer, code, temp)

    source = (ROOT / 'esp32-mini-code/esp32-mini-code.ino').read_text()
    code = source[source.index('#define MAX_SCHEDULES'):source.index('// ── Vibration Motor State')]
    code += r'''
ServerMock server;
struct PreferencesMock {
  std::map<std::string, String> data;
  void begin(const char*, bool) {} void end() {}
  void putInt(const char* key, int v) { data[key] = String(v); }
  void putULong(const char* key, unsigned long v) { data[key] = String(v); }
  void putBool(const char* key, bool v) { data[key] = String(int(v)); }
  void putString(const char* key, const char* v) { data[key] = v; }
  int getInt(const char* key, int v) { return data.count(key) ? data[key].toInt() : v; }
  unsigned long getULong(const char* key, unsigned long v) { return data.count(key) ? data[key].toInt() : v; }
  bool getBool(const char* key, bool v) { return data.count(key) ? data[key].toInt() != 0 : v; }
  String getString(const char* key, const char* v) { return data.count(key) ? data[key] : String(v); }
} preferences;
int lastTriggeredMinute = -1, lastTriggeredHour = -1, lastTriggeredDay = -1;
long gmtOffset_sec = 28800;
unsigned long vibrationStateTimer = 0;
time_t fakeEpoch = 0;
int litSlot = -1, triggerCount = 0, takenCount = 0, missedCount = 0;
void sendCORSHeaders() {}
void startVibrationAlert(int, unsigned long, unsigned long) {}
void stopVibration() {}
void triggerDispenseRelay(int) {}
void notifyArduinoAlert(const String&, int) { triggerCount++; }
void notifyArduinoStop() {}
void notifyDrawerLED(int drawer, int slot, const String& action) {
  int global = drawer == 2 && slot < 21 ? slot + 21 : slot;
  if (action == "alert") litSlot = global;
  else if (action == "clear-alert" && litSlot == global) litSlot = -1;
}
void logDoseEvent(const char*, const char*, const char* resolution, int) {
  if (strcmp(resolution, "TAKEN") == 0) takenCount++;
  if (strcmp(resolution, "MISSED") == 0) missedCount++;
}
time_t getCurrentEpoch() { return fakeEpoch; }
bool getLocalTime(tm* t, int) { time_t local = fakeEpoch + gmtOffset_sec; gmtime_r(&local, t); return true; }
void setTime(int day, int minute) {
  tm t = {}; t.tm_year = 126; t.tm_mon = 9; t.tm_mday = day;
  t.tm_hour = 11; t.tm_min = minute;
  fakeEpoch = timegm(&t) - gmtOffset_sec;
  fakeMillis += 2000;
}
'''
    code += '\n'.join(function(source, name) for name in (
        'saveSchedulesToNVS', 'loadSchedulesFromNVS', 'findScheduleIndex',
        'addOrUpdateSchedule', 'checkAlarms', 'handleStopVibrate', 'compartmentSlot', 'handleAlert'))
    code += r'''
int main() {
  addOrUpdateSchedule("daily45", 11,45,"","Drawer 1 (A1)",0);
  addOrUpdateSchedule("once46", 11,46,"2026-10-30","Drawer 2 (B1)",22);
  addOrUpdateSchedule("daily47", 11,47,"","Drawer 1 (C1)",2);
  addOrUpdateSchedule("future", 11,48,"2026-10-31","Drawer 1 (D1)",3);
  setTime(30,44); checkAlarms(); assert(litSlot == -1);
  setTime(30,45); checkAlarms(); assert(litSlot == 0 && triggerCount == 1);
  setTime(30,45); checkAlarms(); assert(triggerCount == 1);
  setTime(30,46); checkAlarms(); assert(litSlot == 22 && triggerCount == 2);
  assert(takenCount == 0 && !schedules[1].active); // Replacement isn't pickup.
  handleStopVibrate(); assert(litSlot == -1 && takenCount == 1);
  setTime(30,46); checkAlarms(); assert(litSlot == -1);
  // Saving the same dated reminder again must not reactivate it.
  addOrUpdateSchedule("once46", 11,46,"2026-10-30","Drawer 2 (B1)",22);
  assert(!schedules[1].active);
  setTime(30,47); checkAlarms(); assert(litSlot == 2 && triggerCount == 3);
  handleStopVibrate();
  // Simulate a reboot: retained NVS must suppress same-minute daily replay.
  memset(schedules, 0, sizeof(schedules)); scheduleCount = 0;
  lastTriggeredMinute = lastTriggeredHour = lastTriggeredDay = -1;
  loadSchedulesFromNVS();
  setTime(30,47); checkAlarms(); assert(litSlot == -1 && triggerCount == 3);
  setTime(30,48); checkAlarms(); assert(litSlot == -1);
  setTime(31,45); checkAlarms(); assert(litSlot == 0 && triggerCount == 4);
  handleStopVibrate();
  setTime(31,46); checkAlarms(); assert(litSlot == -1); // Dated alarm doesn't repeat.
  setTime(31,47); checkAlarms(); assert(litSlot == 2);
  setTime(31,48); checkAlarms(); assert(litSlot == 3);
  fakeEpoch += 1200; fakeMillis += 1200000; checkAlarms();
  assert(litSlot == -1 && missedCount == 1 && !currentAlert.isAlerting);
  server.args["compartment"] = "Drawer 2 (G3)";
  handleAlert(); assert(litSlot == 41);
  server.args["compartment"] = "Drawer 1 (B1)";
  handleAlert(); assert(litSlot == 1);
}
'''
    run('master', code, temp)
