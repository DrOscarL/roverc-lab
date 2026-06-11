/*
 * ============================================================
 *  RoverC.Pro — Laboratorio Remoto IoT  (FreeRTOS edition)
 *  Hardware : M5StickC Plus2  +  RoverC.Pro HAT
 *
 *  Arquitectura de tasks:
 *    Core 0:
 *      taskMQTT      (prio 3) — WiFi, reconexión, mqtt.loop()
 *      taskTelemetry (prio 2) — sensores + publish cada 500ms
 *      taskDisplay   (prio 1) — refresco TFT cada 300ms
 *      taskButtons   (prio 1) — botones A/B
 *    Core 1:
 *      taskMotors    (prio 4) — desencola y escribe I2C motores
 *      taskServo     (prio 4) — desencola y escribe I2C servo
 *
 *  IPC:
 *    xQueueMotor  — MotorCmd {m1,m2,m3,m4}
 *    xQueueServo  — int angle
 *    xMutexI2C    — protege bus I2C compartido
 *    xMutexState  — protege RoverState compartido
 *
 *  Protocolo I2C RoverC.Pro (0x38):
 *    0x00-0x03  Motor1-4  (int8, ±127)
 *    0x10       Servo1 ángulo (0-180°)
 *
 *  Topics MQTT:
 *    SUB rover/cmd      {"dir":"fwd|bck|left|right|cw|ccw|stop","spd":0-127}
 *    SUB rover/grip     {"angle":0-180}
 *    SUB rover/beep     {"freq":440,"dur":200}
 *    PUB rover/telemetry (cada 500ms)
 *
 *  Librerías:
 *    M5StickCPlus2, PubSubClient, ArduinoJson v7
 * ============================================================
 */

#include <M5StickCPlus2.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Wire.h>

// ─── Credenciales ────────────────────────────────────────────
const char* WIFI_SSID   = "Dr.O";
const char* WIFI_PASS   = "4pfzjyq9thyt";
const char* MQTT_BROKER = "5dd303f2e6fc4da88f4f15cd6837e63a.s1.eu.hivemq.cloud";
const int   MQTT_PORT   = 8883;
const char* MQTT_USER   = "rover_user";
const char* MQTT_PASS   = "PruebaNumero1**";
const char* MQTT_CLIENT = "rovercpro_01";

// Topics
const char* TOPIC_CMD   = "rover/cmd";
const char* TOPIC_GRIP  = "rover/grip";
const char* TOPIC_BEEP  = "rover/beep";
const char* TOPIC_TELEM = "rover/telemetry";

// Hardware
#define ROVER_ADDR      0x38
#define REG_MOTOR1      0x00
#define REG_SERVO       0x10
#define MAX_SPD         127
#define GRIP_OPEN       30
#define GRIP_CLOSED     150
#define BAT_PIN         38

// ─── Tipos para queues ───────────────────────────────────────
struct MotorCmd {
  int8_t m1, m2, m3, m4;
};

// ─── Handles FreeRTOS ────────────────────────────────────────
QueueHandle_t  xQueueMotor;
QueueHandle_t  xQueueServo;
SemaphoreHandle_t xMutexI2C;
SemaphoreHandle_t xMutexState;

// ─── Estado compartido ───────────────────────────────────────
struct RoverState {
  String  dir    = "stop";
  int8_t  spd    = 0;
  int     grip   = GRIP_OPEN;
  float   ax=0, ay=0, az=0;
  float   gx=0, gy=0, gz=0;
  float   batV   = 0;
  int     batPct = 0;
  int32_t rssi   = 0;
  bool    mqttOK = false;
  bool    wifiOK = false;
} state;

// ─── Objetos MQTT (solo usados en taskMQTT) ──────────────────
WiFiClientSecure wifiClient;
PubSubClient     mqttClient(wifiClient);

// ─── I2C helpers (llamar siempre con xMutexI2C tomado) ───────
static void i2c_setMotors(int8_t m1, int8_t m2, int8_t m3, int8_t m4) {
  Wire.beginTransmission(ROVER_ADDR);
  Wire.write(REG_MOTOR1);
  Wire.write((uint8_t)constrain((int)m1, -MAX_SPD, MAX_SPD));
  Wire.write((uint8_t)constrain((int)m2, -MAX_SPD, MAX_SPD));
  Wire.write((uint8_t)constrain((int)m3, -MAX_SPD, MAX_SPD));
  Wire.write((uint8_t)constrain((int)m4, -MAX_SPD, MAX_SPD));
  Wire.endTransmission();
}

static void i2c_setServo(uint8_t angle) {
  Wire.beginTransmission(ROVER_ADDR);
  Wire.write(REG_SERVO);
  Wire.write(constrain((int)angle, 0, 180));
  Wire.endTransmission();
}

// ─── Traducción dir → MotorCmd ───────────────────────────────
MotorCmd dirToMotors(const String& dir, int8_t s) {
  MotorCmd c = {0, 0, 0, 0};
  if      (dir == "fwd")     { c = { s,  s,  s,  s}; }
  else if (dir == "bck")     { c = {-s, -s, -s, -s}; }
  else if (dir == "left")    { c = {-s,  s,  s, -s}; }
  else if (dir == "right")   { c = { s, -s, -s,  s}; }
  else if (dir == "cw")      { c = { s, -s,  s, -s}; }
  else if (dir == "ccw")     { c = {-s,  s, -s,  s}; }
  else if (dir == "diag_fr") { c = { s,  0,  0,  s}; }
  else if (dir == "diag_fl") { c = { 0,  s,  s,  0}; }
  return c;
}

// ─── MQTT callback (ejecuta en contexto de taskMQTT) ─────────
void mqttCallback(char* topic, byte* payload, unsigned int len) {
  JsonDocument doc;
  if (deserializeJson(doc, payload, len)) return;
  String t = String(topic);

  if (t == TOPIC_CMD) {
    String dir = doc["dir"] | "stop";
    int8_t spd = (int8_t)min((int)(doc["spd"] | 0), (int)MAX_SPD);

    // Actualizar estado
    if (xSemaphoreTake(xMutexState, pdMS_TO_TICKS(20))) {
      state.dir = dir;
      state.spd = spd;
      xSemaphoreGive(xMutexState);
    }

    // Encolar comando de motores
    MotorCmd cmd = dirToMotors(dir, spd);
    xQueueOverwrite(xQueueMotor, &cmd);  // overwrite: siempre el comando más reciente
  }
  else if (t == TOPIC_GRIP) {
    int angle = constrain((int)(doc["angle"] | GRIP_OPEN), GRIP_OPEN, GRIP_CLOSED);
    if (xSemaphoreTake(xMutexState, pdMS_TO_TICKS(20))) {
      state.grip = angle;
      xSemaphoreGive(xMutexState);
    }
    xQueueOverwrite(xQueueServo, &angle);
  }
  else if (t == TOPIC_BEEP) {
    int freq = doc["freq"] | 440;
    int dur  = doc["dur"]  | 200;
    M5.Speaker.tone(freq, dur);
  }
}

// ════════════════════════════════════════════════════════════
//  TASK: Motors  (Core 1, prio 4)
// ════════════════════════════════════════════════════════════
void taskMotors(void* pv) {
  MotorCmd cmd;
  for (;;) {
    // Espera indefinida hasta que llegue un comando
    if (xQueueReceive(xQueueMotor, &cmd, portMAX_DELAY)) {
      if (xSemaphoreTake(xMutexI2C, pdMS_TO_TICKS(50))) {
        i2c_setMotors(cmd.m1, cmd.m2, cmd.m3, cmd.m4);
        xSemaphoreGive(xMutexI2C);
      }
    }
  }
}

// ════════════════════════════════════════════════════════════
//  TASK: Servo  (Core 1, prio 4)
// ════════════════════════════════════════════════════════════
void taskServo(void* pv) {
  int angle;
  for (;;) {
    if (xQueueReceive(xQueueServo, &angle, portMAX_DELAY)) {
      if (xSemaphoreTake(xMutexI2C, pdMS_TO_TICKS(50))) {
        i2c_setServo((uint8_t)angle);
        xSemaphoreGive(xMutexI2C);
      }
    }
  }
}

// ════════════════════════════════════════════════════════════
//  TASK: MQTT  (Core 0, prio 3)
// ════════════════════════════════════════════════════════════
void taskMQTT(void* pv) {
  // Configurar cliente TLS
  wifiClient.setInsecure();
  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setBufferSize(512);
  mqttClient.setKeepAlive(30);

  // Conectar WiFi
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("[WiFi] Conectando");
  int retries = 0;
  while (WiFi.status() != WL_CONNECTED && retries++ < 40) {
    vTaskDelay(pdMS_TO_TICKS(500));
    Serial.print(".");
  }
  bool wOK = WiFi.isConnected();
  Serial.printf("\n[WiFi] %s\n", wOK ? WiFi.localIP().toString().c_str() : "FALLO");
  if (xSemaphoreTake(xMutexState, pdMS_TO_TICKS(50))) {
    state.wifiOK = wOK;
    xSemaphoreGive(xMutexState);
  }

  for (;;) {
    // Reconexión MQTT si es necesario
    if (WiFi.isConnected() && !mqttClient.connected()) {
      Serial.println("[MQTT] Conectando...");
      if (mqttClient.connect(MQTT_CLIENT, MQTT_USER, MQTT_PASS)) {
        mqttClient.subscribe(TOPIC_CMD);
        mqttClient.subscribe(TOPIC_GRIP);
        mqttClient.subscribe(TOPIC_BEEP);
        Serial.println("[MQTT] OK");
        M5.Speaker.tone(1047, 100);
        vTaskDelay(pdMS_TO_TICKS(110));
        M5.Speaker.tone(1319, 150);
      } else {
        Serial.printf("[MQTT] Error %d — reintento en 5s\n", mqttClient.state());
        vTaskDelay(pdMS_TO_TICKS(5000));
        continue;
      }
    }

    // Actualizar estado de conexión
    if (xSemaphoreTake(xMutexState, pdMS_TO_TICKS(20))) {
      state.mqttOK = mqttClient.connected();
      state.wifiOK = WiFi.isConnected();
      xSemaphoreGive(xMutexState);
    }

    mqttClient.loop();
    vTaskDelay(pdMS_TO_TICKS(10));  // yield — cede CPU brevemente
  }
}

// ════════════════════════════════════════════════════════════
//  TASK: Telemetría  (Core 0, prio 2)
// ════════════════════════════════════════════════════════════
void taskTelemetry(void* pv) {
  const TickType_t period = pdMS_TO_TICKS(500);
  TickType_t lastWake = xTaskGetTickCount();

  for (;;) {
    vTaskDelayUntil(&lastWake, period);  // periodo exacto, no deriva

    // Leer sensores
    float ax, ay, az, gx, gy, gz;
    M5.Imu.getAccel(&ax, &ay, &az);
    M5.Imu.getGyro (&gx, &gy, &gz);
    int   raw    = analogRead(BAT_PIN);
    float batV   = (raw / 4095.0f) * 3.3f * 2.0f;
    int   batPct = constrain((int)((batV - 3.5f) / 0.7f * 100), 0, 100);
    int32_t rssi = WiFi.RSSI();

    // Actualizar estado compartido
    if (xSemaphoreTake(xMutexState, pdMS_TO_TICKS(30))) {
      state.ax = ax; state.ay = ay; state.az = az;
      state.gx = gx; state.gy = gy; state.gz = gz;
      state.batV   = batV;
      state.batPct = batPct;
      state.rssi   = rssi;
      xSemaphoreGive(xMutexState);
    }

    // Publicar solo si MQTT conectado
    if (!mqttClient.connected()) continue;

    JsonDocument doc;
    doc["bat_v"]   = round(batV * 100) / 100.0f;
    doc["bat_pct"] = batPct;
    doc["ax"] = round(ax * 1000) / 1000.0f;
    doc["ay"] = round(ay * 1000) / 1000.0f;
    doc["az"] = round(az * 1000) / 1000.0f;
    doc["gx"] = round(gx * 10) / 10.0f;
    doc["gy"] = round(gy * 10) / 10.0f;
    doc["gz"] = round(gz * 10) / 10.0f;

    // Leer estado protegido
    if (xSemaphoreTake(xMutexState, pdMS_TO_TICKS(20))) {
      doc["dir"]  = state.dir;
      doc["spd"]  = state.spd;
      doc["grip"] = state.grip;
      xSemaphoreGive(xMutexState);
    }
    doc["rssi"] = rssi;

    char buf[256];
    serializeJson(doc, buf);
    mqttClient.publish(TOPIC_TELEM, buf);
  }
}

// ════════════════════════════════════════════════════════════
//  TASK: Display  (Core 0, prio 1)
// ════════════════════════════════════════════════════════════
void taskDisplay(void* pv) {
  const TickType_t period = pdMS_TO_TICKS(300);
  TickType_t lastWake = xTaskGetTickCount();

  for (;;) {
    vTaskDelayUntil(&lastWake, period);

    // Snapshot del estado (copia local para no mantener mutex durante render)
    RoverState s;
    if (xSemaphoreTake(xMutexState, pdMS_TO_TICKS(30))) {
      s = state;
      xSemaphoreGive(xMutexState);
    } else continue;

    auto& d = M5.Display;
    d.fillScreen(TFT_BLACK);
    d.setTextSize(1);
    d.setTextFont(0);
    d.setCursor(2, 2);

    d.setTextColor(TFT_GREEN);
    d.printf("RoverC FreeRTOS\n");
    d.setTextColor(s.wifiOK ? TFT_GREEN : TFT_RED);
    d.printf("WiFi: %s\n", s.wifiOK ? "OK" : "---");
    d.setTextColor(s.mqttOK ? TFT_GREEN : TFT_RED);
    d.printf("MQTT: %s\n", s.mqttOK ? "OK" : "---");
    d.setTextColor(TFT_WHITE);
    d.printf("BAT: %.2fV (%d%%)\n", s.batV, s.batPct);
    d.printf("RSSI: %d dBm\n", s.rssi);
    d.setTextColor(TFT_CYAN);
    d.printf("DIR: %-6s SPD: %d\n", s.dir.c_str(), s.spd);
    d.printf("GRIP: %d deg\n", s.grip);
    d.setTextColor(TFT_YELLOW);
    d.printf("AX:%.2f AY:%.2f\n", s.ax, s.ay);
    d.printf("AZ:%.2f\n", s.az);
  }
}

// ════════════════════════════════════════════════════════════
//  TASK: Buttons  (Core 0, prio 1)
// ════════════════════════════════════════════════════════════
void taskButtons(void* pv) {
  for (;;) {
    M5.update();

    // Botón A: parar motores
    if (M5.BtnA.wasPressed()) {
      MotorCmd stop = {0, 0, 0, 0};
      xQueueOverwrite(xQueueMotor, &stop);
      if (xSemaphoreTake(xMutexState, pdMS_TO_TICKS(20))) {
        state.dir = "stop";
        state.spd = 0;
        xSemaphoreGive(xMutexState);
      }
      M5.Speaker.tone(440, 100);
      Serial.println("[BTN A] Stop");
    }

    // Botón B: toggle pinza
    if (M5.BtnB.wasPressed()) {
      int angle = GRIP_OPEN;
      if (xSemaphoreTake(xMutexState, pdMS_TO_TICKS(20))) {
        angle = (state.grip == GRIP_OPEN) ? GRIP_CLOSED : GRIP_OPEN;
        state.grip = angle;
        xSemaphoreGive(xMutexState);
      }
      xQueueOverwrite(xQueueServo, &angle);
      M5.Speaker.tone(660, 80);
      Serial.printf("[BTN B] Grip %d\n", angle);
    }

    vTaskDelay(pdMS_TO_TICKS(50));  // 20 Hz de polling de botones
  }
}

// ════════════════════════════════════════════════════════════
//  SETUP
// ════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);

  auto cfg = M5.config();
  M5.begin(cfg);
  pinMode(4, OUTPUT);
  digitalWrite(4, HIGH);  // HOLD — mantener alimentación Plus2

  M5.Display.setRotation(1);
  M5.Display.setTextSize(1);
  M5.Display.setTextColor(TFT_YELLOW);
  M5.Display.setCursor(0, 0);
  M5.Display.println("Iniciando FreeRTOS...");

  M5.Speaker.begin();
  M5.Speaker.setVolume(80);
  M5.Imu.init();
  analogSetAttenuation(ADC_11db);
  analogSetWidth(12);

  // I2C — G0(SDA) G26(SCL) — conector HAT Plus2
  Wire.begin(0, 26);
  Wire.setClock(100000);
  delay(100);

  // I2C Scanner
  Serial.println("=== I2C Scanner ===");
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      Serial.printf("  0x%02X", a);
      if (a == 0x38) Serial.print(" <-- RoverC HAT");
      if (a == 0x68) Serial.print(" <-- IMU");
      Serial.println();
    }
  }
  Serial.println("===================");

  // Crear recursos FreeRTOS
  xQueueMotor  = xQueueCreate(1, sizeof(MotorCmd));  // tamaño 1: solo el último comando
  xQueueServo  = xQueueCreate(1, sizeof(int));
  xMutexI2C    = xSemaphoreCreateMutex();
  xMutexState  = xSemaphoreCreateMutex();

  // Inicializar servo en posición abierta
  if (xSemaphoreTake(xMutexI2C, pdMS_TO_TICKS(100))) {
    i2c_setServo(GRIP_OPEN);
    xSemaphoreGive(xMutexI2C);
  }

  // ── Crear tasks ──────────────────────────────────────────
  //                        función      nombre        stack   param  prio  handle  core
  xTaskCreatePinnedToCore(taskMQTT,      "MQTT",      8192,   NULL,  3,    NULL,   0);
  xTaskCreatePinnedToCore(taskTelemetry, "Telemetry", 4096,   NULL,  2,    NULL,   0);
  xTaskCreatePinnedToCore(taskDisplay,   "Display",   4096,   NULL,  1,    NULL,   0);
  xTaskCreatePinnedToCore(taskButtons,   "Buttons",   2048,   NULL,  1,    NULL,   0);
  xTaskCreatePinnedToCore(taskMotors,    "Motors",    3072,   NULL,  4,    NULL,   1);
  xTaskCreatePinnedToCore(taskServo,     "Servo",     2048,   NULL,  4,    NULL,   1);

  Serial.println("[Setup] Tasks creadas. FreeRTOS scheduler activo.");
  // El scheduler de FreeRTOS ya corre — loop() queda vacío.
}

// ════════════════════════════════════════════════════════════
//  LOOP — vacío: todo corre en tasks FreeRTOS
// ════════════════════════════════════════════════════════════
void loop() {
  vTaskDelete(NULL);  // elimina la task loop() para liberar stack
}
