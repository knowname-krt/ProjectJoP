/*
 * ============================================================================
 *    Plant Watering System — ESP32 Controller
 *
 *    Features:
 *      • AsyncWebServer with REST API
 *      • WebSocket for real-time dashboard updates
 *      • MQTT for cloud-based real-time communication
 *      • Soil moisture sensor (analog)
 *      • Water tank level sensor (analog)
 *      • Pump relay control (ON/OFF)
 *      • Automatic watering schedule
 *      • Manual override
 *
 *    Required Libraries (install via Arduino Library Manager):
 *      1. ESPAsyncWebServer  (by me-no-dev)
 *      2. AsyncTCP           (by me-no-dev)
 *      3. ArduinoJson        (by Benoit Blanchon, v6+)
 *      4. PubSubClient       (by Nick O'Leary)
 *
 *    Board: ESP32 Dev Module
 * ============================================================================
 */

#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <time.h>

// ============================================================================
// CONFIGURATION — CHANGE THESE
// ============================================================================
const char *WIFI_SSID = "YOUR_WIFI_SSID";
const char *WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

// MQTT Broker (must match your web dashboard settings)
const char *MQTT_SERVER = "broker.emqx.io"; // Public test broker
const int MQTT_PORT = 1883;                 // TCP port (not WebSocket)

// MQTT Topics
const char *TOPIC_STATUS = "plant-water/status";     // ESP32 publishes here
const char *TOPIC_COMMAND = "plant-water/command";   // Web publishes here
const char *TOPIC_SCHEDULE = "plant-water/schedule"; // Web publishes here

// ============================================================================
// HARDWARE PINS
// ============================================================================
const int PIN_MOISTURE = 34; // Soil moisture sensor (analog input)
const int PIN_TANK = 35;     // Water level sensor   (analog input)
const int PIN_PUMP = 26;     // Relay / pump          (digital output)
const int PIN_LED = 2;       // Built-in LED (status indicator)

// Calibration values — adjust to your sensors
// Capacitive soil moisture: Air ≈ 3200, Water ≈ 1500
const int MOISTURE_DRY = 3200;
const int MOISTURE_WET = 1500;

// Water tank level: Empty ≈ 3200, Full ≈ 800
const int TANK_EMPTY = 3200;
const int TANK_FULL = 800;

// ============================================================================
// GLOBAL STATE
// ============================================================================
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

// MQTT
WiFiClient mqttWifiClient;
PubSubClient mqtt(mqttWifiClient);

bool pumpOn = false;
bool overrideActive = false;
int moisturePercent = 0;
int tankPercent = 0;

// Schedule
struct Schedule {
  int hour = 7;
  int minute = 0;
  int duration = 10; // seconds
  bool enabled = false;
};

Schedule waterSchedule;
bool scheduleTriggeredToday = false;

// Timing
unsigned long lastBroadcast = 0;
unsigned long lastSensorRead = 0;
const unsigned long BROADCAST_INTERVAL = 2000; // ms
const unsigned long SENSOR_INTERVAL = 1000;    // ms

// ============================================================================
// FORWARD DECLARATIONS
// ============================================================================
String buildStatusJson();
void readSensors();
void broadcastStatus();
void checkSchedule();
void setPump(bool on);
void mqttCallback(char *topic, byte *payload, unsigned int length);
void mqttReconnect();

// ============================================================================
// BUILD JSON STATUS
// ============================================================================
String buildStatusJson() {
  StaticJsonDocument<256> doc;

  doc["moisture"] = moisturePercent;
  doc["tank"] = tankPercent;
  doc["pump"] = pumpOn;

  JsonObject sched = doc.createNestedObject("schedule");
  sched["hour"] = waterSchedule.hour;
  sched["minute"] = waterSchedule.minute;
  sched["duration"] = waterSchedule.duration;
  sched["enabled"] = waterSchedule.enabled;

  String output;
  serializeJson(doc, output);
  return output;
}

// ============================================================================
// SENSOR READING
// ============================================================================
void readSensors() {
  // Soil moisture
  int rawMoisture = analogRead(PIN_MOISTURE);
  moisturePercent = map(rawMoisture, MOISTURE_DRY, MOISTURE_WET, 0, 100);
  moisturePercent = constrain(moisturePercent, 0, 100);

  // Tank level
  int rawTank = analogRead(PIN_TANK);
  tankPercent = map(rawTank, TANK_EMPTY, TANK_FULL, 0, 100);
  tankPercent = constrain(tankPercent, 0, 100);
}

// ============================================================================
// PUMP CONTROL
// ============================================================================
void setPump(bool on) {
  pumpOn = on;
  digitalWrite(PIN_PUMP, on ? HIGH : LOW);
  digitalWrite(PIN_LED, on ? HIGH : LOW);
  Serial.printf("Pump %s\n", on ? "ON" : "OFF");
}

// ============================================================================
// WEBSOCKET EVENTS
// ============================================================================
void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
               AwsEventType type, void *arg, uint8_t *data, size_t len) {

  if (type == WS_EVT_CONNECT) {
    Serial.printf("WS client #%u connected from %s\n", client->id(),
                  client->remoteIP().toString().c_str());
    // Send initial status immediately
    client->text(buildStatusJson());
  } else if (type == WS_EVT_DISCONNECT) {
    Serial.printf("WS client #%u disconnected\n", client->id());
  } else if (type == WS_EVT_DATA) {
    // Handle incoming commands from browser
    AwsFrameInfo *info = (AwsFrameInfo *)arg;
    if (info->opcode == WS_TEXT) {
      String msg = "";
      for (size_t i = 0; i < len; i++)
        msg += (char)data[i];

      StaticJsonDocument<128> cmdDoc;
      DeserializationError err = deserializeJson(cmdDoc, msg);
      if (!err) {
        if (cmdDoc.containsKey("pump")) {
          setPump(cmdDoc["pump"].as<bool>());
        }
      }
    }
  }
}

// ============================================================================
// BROADCAST STATUS TO ALL WEBSOCKET CLIENTS
// ============================================================================
void broadcastStatus() {
  String json = buildStatusJson();
  // WebSocket broadcast
  ws.textAll(json);
  // MQTT publish
  if (mqtt.connected()) {
    mqtt.publish(TOPIC_STATUS, json.c_str());
  }
}

// ============================================================================
// MQTT CALLBACK — receives messages from web dashboard
// ============================================================================
void mqttCallback(char *topic, byte *payload, unsigned int length) {
  String message = "";
  for (unsigned int i = 0; i < length; i++) {
    message += (char)payload[i];
  }

  Serial.printf("MQTT [%s]: %s\n", topic, message.c_str());

  // Handle command topic
  if (String(topic) == TOPIC_COMMAND) {
    StaticJsonDocument<128> doc;
    DeserializationError err = deserializeJson(doc, message);
    if (!err) {
      String action = doc["action"] | "";
      if (action == "pump_on") {
        setPump(true);
        Serial.println("MQTT → Pump ON");
      } else if (action == "pump_off") {
        setPump(false);
        Serial.println("MQTT → Pump OFF");
      }
    } else {
      // Simple text commands
      if (message == "water" || message == "on") {
        setPump(true);
        delay(2000);
        setPump(false);
      }
    }
  }

  // Handle schedule topic
  if (String(topic) == TOPIC_SCHEDULE) {
    StaticJsonDocument<128> doc;
    DeserializationError err = deserializeJson(doc, message);
    if (!err) {
      waterSchedule.hour = doc["hour"] | 7;
      waterSchedule.minute = doc["minute"] | 0;
      waterSchedule.duration = doc["duration"] | 10;
      waterSchedule.enabled = doc["enabled"] | true;
      Serial.printf("MQTT → Schedule set: %02d:%02d for %ds\n",
                    waterSchedule.hour, waterSchedule.minute,
                    waterSchedule.duration);
    }
  }
}

// ============================================================================
// MQTT RECONNECT
// ============================================================================
void mqttReconnect() {
  if (mqtt.connected())
    return;

  String clientId = "esp32-plant-" + String(random(0xffff), HEX);
  Serial.printf("MQTT connecting as %s... ", clientId.c_str());

  if (mqtt.connect(clientId.c_str())) {
    Serial.println("connected ✓");
    mqtt.subscribe(TOPIC_COMMAND);
    mqtt.subscribe(TOPIC_SCHEDULE);
  } else {
    Serial.printf("failed, rc=%d\n", mqtt.state());
  }
}

// ============================================================================
// CHECK WATERING SCHEDULE
// ============================================================================
void checkSchedule() {
  if (!waterSchedule.enabled)
    return;

  struct tm timeinfo;
  if (!getLocalTime(&timeinfo))
    return;

  int currentHour = timeinfo.tm_hour;
  int currentMin = timeinfo.tm_min;

  if (currentHour == waterSchedule.hour && currentMin == waterSchedule.minute &&
      !scheduleTriggeredToday) {

    Serial.println("Schedule triggered — watering...");
    scheduleTriggeredToday = true;
    setPump(true);

    // Non-blocking: we'll turn it off after duration in loop()
    // For simplicity, use a blocking delay here
    delay(waterSchedule.duration * 1000);
    setPump(false);
    Serial.println("Scheduled watering complete.");
  }

  // Reset trigger flag at the start of the next minute
  if (currentMin != waterSchedule.minute) {
    scheduleTriggeredToday = false;
  }
}

// ============================================================================
// SETUP
// ============================================================================
void setup() {
  Serial.begin(115200);
  Serial.println("\n=============================");
  Serial.println("  Plant Watering System");
  Serial.println("  ESP32 Controller v2.0");
  Serial.println("=============================\n");

  // Pin modes
  pinMode(PIN_MOISTURE, INPUT);
  pinMode(PIN_TANK, INPUT);
  pinMode(PIN_PUMP, OUTPUT);
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_PUMP, LOW);
  digitalWrite(PIN_LED, LOW);

  // ── WiFi ──
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to WiFi");

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 40) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\nWiFi connected!  IP: %s\n",
                  WiFi.localIP().toString().c_str());
  } else {
    Serial.println("\nWiFi connection FAILED. Restarting...");
    ESP.restart();
  }

  // ── NTP Time Sync (for schedule) ──
  configTime(7 * 3600, 0, "pool.ntp.org", "time.nist.gov"); // UTC+7
  Serial.println("NTP time sync requested...");

  // ── WebSocket ──
  ws.onEvent(onWsEvent);
  server.addHandler(&ws);

  // ── MQTT Setup ──
  mqtt.setServer(MQTT_SERVER, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  mqtt.setBufferSize(512);

  // ── REST API Endpoints ──

  // GET /  — Serve the HTML dashboard
  // (In production, you'd serve index.html from SPIFFS/LittleFS)
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "text/plain",
                  "Plant Watering System ESP32 is running.\n"
                  "Open the index.html dashboard and point it to this IP.\n"
                  "API: /status, /moisture, /pump/on, /pump/off, /schedule");
  });

  // GET /status — Full system status (JSON)
  server.on("/status", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "application/json", buildStatusJson());
  });

  // GET /moisture — Just the moisture reading
  server.on("/moisture", HTTP_GET, [](AsyncWebServerRequest *request) {
    String json = "{\"moisture\":" + String(moisturePercent) + "}";
    request->send(200, "application/json", json);
  });

  // GET /pump/on — Turn pump ON
  server.on("/pump/on", HTTP_GET, [](AsyncWebServerRequest *request) {
    setPump(true);
    request->send(200, "application/json", "{\"pump\":true,\"status\":\"ok\"}");
  });

  // GET /pump/off — Turn pump OFF
  server.on("/pump/off", HTTP_GET, [](AsyncWebServerRequest *request) {
    setPump(false);
    request->send(200, "application/json",
                  "{\"pump\":false,\"status\":\"ok\"}");
  });

  // POST /schedule — Set watering schedule
  server.on(
      "/schedule", HTTP_POST,
      // onRequest handler (for non-body requests)
      [](AsyncWebServerRequest *request) {
        request->send(400, "application/json", "{\"error\":\"missing body\"}");
      },
      // onUpload handler (unused)
      NULL,
      // onBody handler
      [](AsyncWebServerRequest *request, uint8_t *data, size_t len,
         size_t index, size_t total) {
        String body = "";
        for (size_t i = 0; i < len; i++)
          body += (char)data[i];

        StaticJsonDocument<128> doc;
        DeserializationError err = deserializeJson(doc, body);

        if (err) {
          request->send(400, "application/json",
                        "{\"error\":\"invalid json\"}");
          return;
        }

        waterSchedule.hour = doc["hour"] | 7;
        waterSchedule.minute = doc["minute"] | 0;
        waterSchedule.duration = doc["duration"] | 10;
        waterSchedule.enabled = doc["enabled"] | true;

        Serial.printf("Schedule set: %02d:%02d for %ds\n", waterSchedule.hour,
                      waterSchedule.minute, waterSchedule.duration);

        request->send(200, "application/json",
                      "{\"status\":\"ok\",\"schedule\":{\"hour\":" +
                          String(waterSchedule.hour) +
                          ",\"minute\":" + String(waterSchedule.minute) +
                          ",\"duration\":" + String(waterSchedule.duration) +
                          ",\"enabled\":" +
                          String(waterSchedule.enabled ? "true" : "false") +
                          "}}");
      });

  // Handle CORS preflight
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Methods",
                                       "GET, POST, OPTIONS");
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Headers",
                                       "Content-Type");

  // Start server
  server.begin();
  Serial.println("HTTP + WebSocket server started on port 80");
  Serial.printf("Dashboard: http://%s\n", WiFi.localIP().toString().c_str());
}

// ============================================================================
// LOOP
// ============================================================================
void loop() {
  // Clean up disconnected WebSocket clients
  ws.cleanupClients();

  // Keep MQTT connection alive
  if (!mqtt.connected()) {
    mqttReconnect();
  }
  mqtt.loop();

  unsigned long now = millis();

  // Read sensors periodically
  if (now - lastSensorRead >= SENSOR_INTERVAL) {
    lastSensorRead = now;
    readSensors();
  }

  // Broadcast status to all WS + MQTT clients periodically
  if (now - lastBroadcast >= BROADCAST_INTERVAL) {
    lastBroadcast = now;
    broadcastStatus();
  }

  // Check watering schedule
  checkSchedule();
}
