# ESP1 - Drawer 1 (21 LEDs Controller) Firmware

This firmware runs on an **ESP32** or **ESP8266** (e.g. NodeMCU, Wemos D1 Mini, or SuperMini) to control the **21 compartment LEDs of Drawer 1** in the Smart Medicine Dispenser.

---

## 💡 Overview & Compartment Mapping

Drawer 1 consists of a 7-column by 3-row grid (**21 total compartments**):

| Row \ Col | Col A | Col B | Col C | Col D | Col E | Col F | Col G |
| :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: |
| **Row 1** | **A1** (Slot 0) | **B1** (Slot 1) | **C1** (Slot 2) | **D1** (Slot 3) | **E1** (Slot 4) | **F1** (Slot 5) | **G1** (Slot 6) |
| **Row 2** | **A2** (Slot 7) | **B2** (Slot 8) | **C2** (Slot 9) | **D2** (Slot 10) | **E2** (Slot 11) | **F2** (Slot 12) | **G2** (Slot 13) |
| **Row 3** | **A3** (Slot 14) | **B3** (Slot 15) | **C3** (Slot 16) | **D3** (Slot 17) | **E3** (Slot 18) | **F3** (Slot 19) | **G3** (Slot 20) |

---

## 🎨 LED Color States

1. **Active / In Stock (`GREEN`)**:
   - Compartment has an active scheduled reminder and pill stock is available (1–7 doses).
2. **Out of Stock (`RED`)**:
   - 0/7 doses remaining (weekly 7-day cycle completed). Flags that restocking is required.
3. **Due Now / Alerting (`PULSING AMBER / ORANGE`)**:
   - The scheduled alarm time has arrived; pulses during the 20-minute countdown window to guide the patient directly to the correct compartment.
4. **Inactive (`OFF`)**:
   - Empty compartment with no active schedule.

---

## 🔌 Hardware Wiring & Pinout

Using a standard **WS2812B Addressable RGB LED Strip** (or individual WS2812B pixel LEDs daisy-chained under each compartment):

| Wire | ESP32 Pin | ESP8266 Pin | WS2812B Strip Pin | Note |
| :--- | :--- | :--- | :--- | :--- |
| **5V / VCC** | `5V` / `VIN` | `5V` / `VIN` | `+5V` | Powered by 5V 2A Power Supply |
| **GND** | `GND` | `GND` | `GND` | Common Ground |
| **DATA** | **`GPIO 4`** | **`D2` (`GPIO 4`)** | **`DIN`** | Via 330Ω - 470Ω Resistor |

### Wiring Diagram:
```
ESP (GPIO 4) ───[ 330Ω Resistor ]─── DIN ───► [ LED 0 (A1) ] ──► [ LED 1 (B1) ] ... ──► [ LED 20 (G3) ]
5V Power Supply (+) ──────────────── +5V ────────────────────────────────────────────────────────┘
5V Power Supply (-) ──────────────── GND (Connect ESP GND + Strip GND together)
```

---

## 📡 Wi-Fi HTTP REST API (Port 80)

All endpoints support CORS for direct browser and mobile app calls:

| Endpoint | Method | Query Parameters | Description |
| :--- | :--- | :--- | :--- |
| `/status` | `GET` | - | Returns JSON status of all 21 LEDs |
| `/led` | `GET` | `slot=0&state=active` or `comp=A1&color=green` | Set state: `active` (green), `out_of_stock` (red), `alert` (blinking), `off` |
| `/alert` | `GET` | `slot=0` or `comp=A1` | Starts flashing amber alert on that compartment |
| `/clear-alert`| `GET` | `slot=0` or `comp=A1` | Clears flashing alert (returns to green) |
| `/clear` | `GET` | - | Turns off all 21 LEDs |
| `/test` | `GET` | - | Runs a sequential green/red sweep animation |

### Example Wi-Fi Calls:
```bash
# Turn slot 0 (A1) Green (In-stock schedule)
curl "http://dispenser-drawer1.local/led?slot=0&state=active"

# Turn slot 0 (A1) Red (Out of stock)
curl "http://dispenser-drawer1.local/led?slot=0&state=out_of_stock"

# Start flashing alert on slot 0 (A1) during 20-min countdown
curl "http://dispenser-drawer1.local/alert?slot=0"

# Clear all LEDs
curl "http://dispenser-drawer1.local/clear"
```

---

## 🛠️ Upload Instructions (Arduino IDE)

1. Open Arduino IDE.
2. Install **`Adafruit NeoPixel`** from **Tools ➔ Manage Libraries**.
3. Select board:
   - For ESP32: **`ESP32 Dev Module`** or **`WEMOS LOLIN D1 MINI ESP32`**
   - For ESP8266: **`NodeMCU 1.0`** or **`LOLIN(WEMOS) D1 R2 & mini`**
4. Set Wi-Fi credentials in lines 39–40 of `esp1.ino`:
   ```cpp
   const char* ssid     = "YOUR_WIFI_SSID";
   const char* password = "YOUR_WIFI_PASSWORD";
   ```
5. Click **Upload**.
