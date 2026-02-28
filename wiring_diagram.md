# Wiring Diagram — Plant Watering System (ESP32)

## Components

| # | Component | Qty | Notes |
|---|-----------|-----|-------|
| 1 | ESP32 Dev Board | 1 | Any ESP32 variant (DevKitC, NodeMCU-32S, etc.) |
| 2 | Capacitive Soil Moisture Sensor v1.2 | 1 | Analog output (avoid resistive type) |
| 3 | Water Level Sensor | 1 | Analog output (or ultrasonic HC-SR04) |
| 4 | 5V Relay Module (1-channel) | 1 | Opto-isolated recommended |
| 5 | Mini Water Pump (3-6V DC) | 1 | Submersible |
| 6 | Power Supply 5V 2A | 1 | USB or barrel jack |
| 7 | Jumper Wires | ~12 | Male-to-female |
| 8 | Water Container / Tank | 1 | Any container for water |

---

## Wiring Connections

```
┌─────────────────────────────────────────────────────────────┐
│                        ESP32 Board                          │
│                                                             │
│   3V3 ──────────────┬─── Moisture Sensor VCC                │
│                     └─── Water Level Sensor VCC             │
│                                                             │
│   GND ──────────────┬─── Moisture Sensor GND                │
│                     ├─── Water Level Sensor GND             │
│                     └─── Relay GND                          │
│                                                             │
│   GPIO 34 (ADC) ────── Moisture Sensor AOUT                │
│                                                             │
│   GPIO 35 (ADC) ────── Water Level Sensor AOUT             │
│                                                             │
│   GPIO 26 ──────────── Relay IN (Signal)                    │
│                                                             │
│   VIN (5V) ─────────── Relay VCC                            │
│                                                             │
└─────────────────────────────────────────────────────────────┘

┌───────────────┐         ┌───────────────┐
│  Relay Module │         │   Water Pump  │
│               │         │               │
│  COM ─────────┼─── (+)──┤  Motor +      │
│  NO  ─────────┼─── via──┤               │
│               │  power  │  Motor -  ────┤──── Power Supply (-)
│  VCC ← 5V    │  supply  │               │
│  GND ← GND   │   (+)   │               │
│  IN  ← GPIO26│         │               │
└───────────────┘         └───────────────┘
```

### Pin Summary

| ESP32 Pin | Connected To | Type |
|-----------|-------------|------|
| **GPIO 34** | Moisture Sensor AOUT | Analog Input |
| **GPIO 35** | Water Level Sensor AOUT | Analog Input |
| **GPIO 26** | Relay IN | Digital Output |
| **GPIO 2** | Built-in LED (status) | Digital Output |
| **3V3** | Sensor VCC (both) | Power |
| **VIN** | Relay VCC | Power (5V) |
| **GND** | Common ground | Ground |

---

## Important Notes

> ⚠️ **GPIO 34 & 35** are input-only on ESP32 — they cannot be used as outputs. This is correct since they're connected to analog sensors.

> ⚠️ **Relay module**: The relay isolates the pump's power circuit from the ESP32. Use the **NO** (Normally Open) terminal so the pump is OFF by default.

> ⚠️ **Pump power**: Do NOT power the pump directly from the ESP32. Use an external 5V power supply through the relay.

---

## Sensor Calibration

After wiring, calibrate in Arduino Serial Monitor:

1. **Moisture sensor in air** → note the raw value (e.g., `3200`) → set as `MOISTURE_DRY`
2. **Moisture sensor in water** → note the raw value (e.g., `1500`) → set as `MOISTURE_WET`
3. **Tank empty** → note raw value → set as `TANK_EMPTY`
4. **Tank full** → note raw value → set as `TANK_FULL`

---

## JSON API Examples

### `GET /status` — Full System Status

```json
{
  "moisture": 65,
  "tank": 78,
  "pump": false,
  "schedule": {
    "hour": 7,
    "minute": 0,
    "duration": 10,
    "enabled": true
  }
}
```

### `GET /moisture` — Moisture Only

```json
{
  "moisture": 65
}
```

### `GET /pump/on` — Turn Pump ON

```json
{
  "pump": true,
  "status": "ok"
}
```

### `GET /pump/off` — Turn Pump OFF

```json
{
  "pump": false,
  "status": "ok"
}
```

### `POST /schedule` — Set Watering Schedule

**Request:**
```json
{
  "hour": 7,
  "minute": 30,
  "duration": 15,
  "enabled": true
}
```

**Response:**
```json
{
  "status": "ok",
  "schedule": {
    "hour": 7,
    "minute": 30,
    "duration": 15,
    "enabled": true
  }
}
```

### WebSocket (`ws://ESP32_IP/ws`)

Broadcasts every 2 seconds:
```json
{
  "moisture": 52,
  "tank": 85,
  "pump": false,
  "schedule": {
    "hour": 7,
    "minute": 0,
    "duration": 10,
    "enabled": false
  }
}
```
