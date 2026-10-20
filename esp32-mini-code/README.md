# Smart Medicine Dispenser - Hardware Firmware Suite

This directory contains the firmware for the **Master ESP32 Mini Controller**, which coordinates the entire physical dispenser system alongside **ESP1 (Drawer 1 LEDs)**, **ESP2 (Drawer 2 LEDs)**, and the **Arduino (LCD & Relays)**.

---

## 🏗️ Hardware Architecture Overview

```
                 [ Mobile App (Wi-Fi HTTP Client) ]
                                 │
                                 ▼
                   (Local Wi-Fi Network / 2.4G)
                                 │
                 ┌───────────────┴───────────────┐
                 │                               │
                 ▼                               ▼
       [ ESP1 (Drawer 1) ]             [ ESP2 (Drawer 2) ]
        • 21 RGB LEDs (WS2812B)         • 21 RGB LEDs (WS2812B)
        • Compartments A1..G3           • Compartments A1..G3
        • Green = In Stock              • Green = In Stock
        • Red = Out of Stock            • Red = Out of Stock
        • Amber Pulse = Alerting        • Amber Pulse = Alerting
                 ▲                               ▲
                 │ (HTTP /led /alert /clear)     │ (HTTP /led /alert /clear)
                 │                               │
         ┌───────┴───────────────────────────────┴───────┐
         │       ESP32 MINI MASTER CONTROLLER            │
         │  • Wi-Fi NTP Real-Time Clock & Epoch Sync     │
         │  • Non-blocking Vibration Motor Alert (GPIO 4)│
         │  • 20-Minute Grace Period State Machine       │
         │  • Event History Buffer (GET /events)         │
         │  • NVS Flash Memory (Saved Schedules)         │
         └───────────────────────┬───────────────────────┘
                                 │
                                 │ (UART Serial: DISPENSE / ALERT / TAKEN)
                                 ▼
                     [ ARDUINO CONTROLLER ]
                      • 16x2 I2C LCD Display
                      • 4-Channel Dispense Relays
```

---

## 🔌 ESP32 Mini Hardware Wiring

| Function | Default ESP32 Pin | Connected Component | Description |
|---|---|---|---|
| **Vibration Motor** | **`GPIO 4`** | NPN Transistor Base / Driver `IN` | Triggers alert vibration pulses |
| **Status LED** | **`GPIO 2`** (or `GPIO 8` on C3) | Onboard LED | Visual status / heartbeat |
| **Serial TX** | **`TX0` (GPIO 1)** | Arduino `RX` (Pin 0) | Sends `DISPENSE`, `ALERT`, `TAKEN` |
| **Serial RX** | **`RX0` (GPIO 3)** | Arduino `TX` (Pin 1) | Receives acknowledgments from Arduino |
| **GND** | **`GND`** | System Common GND | Connected to all boards |

### Vibration Motor Driver Circuit:
```
ESP32 GPIO 4 ───[ 1kΩ Resistor ]─── Base (NPN 2N2222)
                                      │
                                    Emitter ─── GND
                                      │
                                   Collector
                                      │
                                      ├──[ Vibration Motor (-) ]
                                      │       │
                                   [Diode 1N4001]
                                      │       │
                                      └──[ Vibration Motor (+) ]─── +3.3V or +5V
```
> **Tip:** If using a 3-pin vibration motor module, connect `VCC -> 5V`, `GND -> GND`, and `IN -> GPIO 4`.

---

## ⏱️ 20-Minute Active Alert Lifecycle (Specification Verified)

1. **Alarm Trigger (Scheduled Time Hit):**
   * ESP32 Mini enters active alert (`is_alerting: true`, `remaining_seconds: 1200`).
   * Vibration motor fires **3 strong pulses** (`700ms on / 300ms off`).
   * Sends HTTP trigger to **ESP1** (if Drawer 1) or **ESP2** (if Drawer 2) to flash the compartment LED amber.
   * Sends `ALERT:<compartment>` to the Arduino to display on the 16x2 LCD.

2. **Periodic Reminder During 20 Minutes:**
   * Every **3 minutes**, fires a 2-pulse vibration reminder if not yet taken.

3. **Resolution A - Patient Takes Dose (On-Time):**
   * Patient taps "I Have Taken This" or opens drawer (`GET /stop-vibrate` or `POST /dispense-confirm`).
   * Vibration stops instantly.
   * Compartment LED returns to steady Green.
   * Arduino LCD displays `"DOSE TAKEN! :)"`.
   * Logged as **`TAKEN`** with exact elapsed seconds in `GET /events`.

4. **Resolution B - 20 Minutes Timeout (Missed):**
   * If 1200 seconds elapse without intake:
   * Vibration stops.
   * Compartment LED clears.
   * Arduino LCD displays `"DOSE MISSED :("`.
   * Logged as **`MISSED`** in `GET /events`.

---

## 📡 REST API Reference (Port 80)

| Method | Endpoint | Description | Example Query |
|---|---|---|---|
| `GET` | `/status` | Complete state with `active_alert` and countdown | `http://dispenser.local/status` |
| `GET` | `/events` | Last 20 dose events for offline mobile sync | `http://dispenser.local/events?limit=10` |
| `GET/POST`| `/vibrate` | Trigger custom vibration pattern | `http://dispenser.local/vibrate?duration=2500&pulses=3` |
| `GET` | `/stop-vibrate` | Stop vibration and mark dose taken | `http://dispenser.local/stop-vibrate` |
| `GET/POST`| `/set-schedule` | Add or update schedule with RTC drift calibration | `http://dispenser.local/set-schedule?hour=8&minute=30&amPm=AM&slotIndex=0` |
| `GET` | `/delete-schedule`| Delete schedule by ID | `http://dispenser.local/delete-schedule?id=sch_1` |
| `GET` | `/schedules` | Returns all schedules stored in flash | `http://dispenser.local/schedules` |
| `GET` | `/dispense` | Pulse relay & vibrate | `http://dispenser.local/dispense?relay=1` |

---

## 🛠️ Upload Instructions

1. Open `esp32-mini-code.ino` in Arduino IDE.
2. Select board: **`ESP32 Dev Module`** (or **`ESP32C3 Dev Module`** for SuperMini).
3. Update Wi-Fi SSID and password in lines 21–22.
4. Click **Upload**.
