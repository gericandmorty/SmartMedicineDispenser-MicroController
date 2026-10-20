# Arduino Controller - LCD, Relays, Solenoid & Magnetic Reed Switches

This Arduino code controls the physical user interface and feedback mechanisms of the Smart Medicine Dispenser, connecting with the **ESP32-Mini Master** (which provides Wi-Fi and connects to the Mobile App).

---

## 🔌 Complete Hardware Pinout (Arduino Uno R3)

| Component | Arduino Pin | Description |
|---|---|---|
| **I2C LCD 16x2 (SDA)** | **`A4`** (or dedicated SDA) | I2C Data line for LCD display |
| **I2C LCD 16x2 (SCL)** | **`A5`** (or dedicated SCL) | I2C Clock line for LCD display |
| **Buzzer Relay (1 GPIO)**| **`Pin 7`** | Controls Cabinet Buzzer Beeping Alert (Active LOW) |
| **Solenoid Lock (1 GPIO)**| **`Pin 8`** | Unlocks drawer/compartment during medication time |
| **Magnetic Reed Switch 1** | **`Pin 2`** | Drawer 1 Open/Closed Sensor (`INPUT_PULLUP` to GND) |
| **Magnetic Reed Switch 2** | **`Pin 3`** | Drawer 2 Open/Closed Sensor (`INPUT_PULLUP` to GND) |
| **Serial RX** | **`Pin 0 (RX)`** | Connected to ESP32-Mini `TX` |
| **Serial TX** | **`Pin 1 (TX)`** | Connected to ESP32-Mini `RX` |
| **Common GND** | **`GND`** | Connected to ESP32 GND and power supply GND |

---

## ⚡ Wiring Circuit Details

### 1. 16x2 I2C LCD Display
* `VCC` -> Arduino `5V`
* `GND` -> Arduino `GND`
* `SDA` -> Arduino `A4`
* `SCL` -> Arduino `A5`

### 2. Buzzer Relay Module (1 GPIO)
* `VCC` -> `5V`
* `GND` -> `GND`
* `IN`  -> Arduino **`Pin 7`**
* Output contacts -> Controls the active buzzer power.

### 3. Solenoid Lock (1 GPIO)
* Signal -> Arduino **`Pin 8`** (via MOSFET/Transistor driver or Relay channel)
* Solenoid power -> 12V / 5V external power supply (ensure common GND).

### 4. Magnetic Reed Switches (2 GPIOs)
* **Drawer 1 Reed Switch:** One terminal to **`Pin 2`**, other terminal to **`GND`**.
* **Drawer 2 Reed Switch:** One terminal to **`Pin 3`**, other terminal to **`GND`**.
* Internal pull-up resistors are enabled in software (`INPUT_PULLUP`).
* When drawer is pulled open, magnet separates from switch -> triggers dose **TAKEN**!

### 5. Arduino to ESP32-Mini Serial Link
```
Arduino Pin 0 (RX) <──── ESP32 TX
Arduino Pin 1 (TX) ────> ESP32 RX
Arduino GND        <──── ESP32 GND (Shared Common Ground)
```

---

## 🔄 End-to-End Operation (Matching Flowchart)

1. **Schedule Trigger:**
   * ESP32-Mini matches schedule time -> sends `ALERT:Drawer 1 (A1)` over Serial to Arduino.
   * Arduino turns on **Buzzer Relay** (beeps cadence).
   * Arduino energizes **Solenoid** to unlock the drawer.
   * LCD begins displaying **20-minute live countdown**:
     ```
     Line 1: DUE: Drawer 1(A1)
     Line 2: 19m 58s left
     ```

2. **User Takes Medicine (Reed Switch Opens):**
   * Patient opens Drawer 1.
   * Magnetic reed switch on Pin 2 triggers.
   * Arduino **instantly silences buzzer relay** and locks solenoid.
   * LCD displays:
     ```
     Line 1: DOSE TAKEN! :)
     Line 2: Drawer 1 Opened
     ```
   * Arduino sends `TAKEN:Drawer 1` over Serial to ESP32-Mini.
   * ESP32-Mini stops vibration, turns off alerting LED on ESP1/ESP2, and marks status **`TAKEN`** on the Mobile App!

3. **20-Minute Timeout (User Misses Dose):**
   * If 20 minutes elapse without drawer opening:
   * Arduino shuts off buzzer relay and locks solenoid.
   * LCD displays:
     ```
     Line 1: DOSE MISSED :(
     Line 2: Drawer 1 (A1)
     ```
   * Arduino sends `MISSED:Drawer 1` to ESP32-Mini.
   * Mobile app records dose as **`MISSED`**.
