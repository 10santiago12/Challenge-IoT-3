#include <WiFi.h>
#include <WebServer.h>
#include <PubSubClient.h>  // Biblioteca MQTT
#include <Wire.h>
#include <MPU6500_WE.h>
#include <LiquidCrystal.h>
#include <math.h>

// ===== Pines (fila de abajo) =====
// Sensores/actuadores
#define PIN_HUM    32     // ADC humedad (A0 del módulo resistivo)
#define PIN_RAIN   15    // MH-RD D0 (activo LOW con pull-up)
#define PIN_LED    23    // LED
#define PIN_BUZZ   5     // Buzzer

// I2C IMU dedicado (NO usar 21/22)
#define I2C_SDA    19
#define I2C_SCL    18

// LCD 16x2 (paralelo 4-bit)  RS, E, D4, D5, D6, D7
#define LCD_RS     22
#define LCD_E      21
#define LCD_D4     2
#define LCD_D5     16     // RX2
#define LCD_D6     17     // TX2
#define LCD_D7     3      // RX0

LiquidCrystal lcd(LCD_RS, LCD_E, LCD_D4, LCD_D5, LCD_D6, LCD_D7);

// ===== Configuración WiFi y MQTT =====
const char* WIFI_SSID = "Hotspot3";          // SSID de tu red WiFi
const char* WIFI_PASS = "12345678";          // Contraseña de tu red
const char* MQTT_BROKER = "192.168.137.101"; // IP de la Raspberry Pi en tu red
const int   MQTT_PORT = 1883;
const char* MQTT_TOPIC_SENSORS = "talud/sensors";
const char* MQTT_TOPIC_CONTROL = "talud/control/silence";

WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);

// ===== Config lluvia =====
#define RAIN_ACTIVE_LOW   1
#define RAIN_CONFIRM_MS   1000

// ===== IMU =====
MPU6500_WE mpu(0x68);     // AD0 a GND => 0x68

// ===== Calibraciones (ajusta con tus lecturas reales) =====
int   humDry     = 3000;  // ADC en seco real
int   humWet     = 1200;  // ADC en suelo saturado real
float rmsBase    = 0.02f; // g
float rmsSigma   = 0.01f; // g
float THR_V_PEAK = 0.20f; // g pico

// ===== Estado lluvia =====
volatile bool rainRaw    = false;
volatile bool rainActive = false;
volatile unsigned long rainSince = 0;

// ===== Silenciar alarmas (toggle desde web) =====
volatile bool alarmSilenced = false;

// ===== Mutex para protección de datos compartidos =====
SemaphoreHandle_t dataMutex;

// ===== Histórico en RAM =====
#define HISTORY_SIZE 60
struct Sample {
  unsigned long t;
  float H;          // 0..1
  bool rainRaw;
  bool rainActive;
  float rms;        // g
  float peak;       // g
  float Score;      // 0..1
  int level;        // 0/1/2
};
Sample history[HISTORY_SIZE];
int histIndex = 0;
int histCount = 0;

// ===== Últimos valores para UI/LCD =====
volatile float lastH = 0.0f;
volatile float lastRMS = 0.0f;
volatile float lastPeak = 0.0f;
volatile float lastScore = 0.0f;
volatile int   lastLevel = 0;

// ===== NUEVO: Inclinación =====
volatile float lastPitch = 0.0f; // grados
volatile float lastRoll  = 0.0f; // grados
static inline float lowpass(float prev, float now, float alpha=0.2f){
  return prev + alpha*(now-prev);
}

// ===== Handles de las tareas (hilos) =====
TaskHandle_t humidityTaskHandle = NULL;
TaskHandle_t rainTaskHandle = NULL;
TaskHandle_t imuTaskHandle = NULL;
TaskHandle_t scoreTaskHandle = NULL;
TaskHandle_t actuatorsTaskHandle = NULL;
TaskHandle_t mqttTaskHandle = NULL;

// ===== Utils =====
static inline float clamp01(float x){ return x<0?0:(x>1?1:x); }

// ---------- Lluvia (debounce temporal) ----------
void updateRainState_raw() {
  int val = digitalRead(PIN_RAIN);
  bool nowWet = RAIN_ACTIVE_LOW ? (val == LOW) : (val == HIGH);
  unsigned long t = millis();
  if (nowWet) {
    if (!rainRaw) rainSince = t;                     // flanco a mojado
    rainActive = (t - rainSince >= RAIN_CONFIRM_MS); // confirma
  } else {
    rainActive = false;
  }
  rainRaw = nowWet;
}

// ---------- Humedad (mediana de 3 + mapeo seco/mojado) ----------
float readHumidityNorm_raw() {
  // Mejor rango ADC para 0..3.3V
  // (hazlo una sola vez en setup también; repetir no daña)
  analogSetPinAttenuation(PIN_HUM, ADC_11db);

  int a1 = analogRead(PIN_HUM);
  int a2 = analogRead(PIN_HUM);
  int a3 = analogRead(PIN_HUM);
  // mediana
  int lo = min(a1, min(a2, a3));
  int hi = max(a1, max(a2, a3));
  int adc = a1 + a2 + a3 - lo - hi;

  // clamps duros para evitar 100% fijo si el sensor satura
  if (adc >= humDry) return 0.0f;   // seco
  if (adc <= humWet) return 1.0f;   // mojado

  int span = humDry - humWet;       // >0 si humDry>humWet
  float h = (span == 0) ? 0.0f : (float)(humDry - adc) / (float)span;
  return clamp01(h);
}

// ---------- Vibración (RMS y pico en ~1 s) ----------
float readIMURMS_1s(float &peakOut) {
  unsigned long t0 = millis();
  float sum = 0.0f, peak = 0.0f;
  int n = 0;
  while (millis() - t0 < 1000) {
    xyzFloat a = mpu.getGValues();          // en g
    float mag_g = sqrtf(a.x*a.x + a.y*a.y + a.z*a.z);
    float dyn_g = mag_g - 1.0f;             // remueve gravedad
    sum += dyn_g * dyn_g;
    float absdyn = fabsf(dyn_g);
    if (absdyn > peak) peak = absdyn;
    n++;
    delay(10); // ~100 Hz
  }
  peakOut = peak;
  return (n>0) ? sqrtf(sum / (float)n) : 0.0f;
}

// ============================================================================
// TAREAS (HILOS) FREERTOS - Cumple con "medición desde hilos distintos al principal"
// ============================================================================

// ---------- TAREA 1: Medición de Humedad (hilo independiente) ----------
void humidityTask(void *pvParameters) {
  (void) pvParameters;
  
  for (;;) {
    // Leer humedad normalizada (0..1)
    float H = readHumidityNorm_raw();
    
    // Actualizar variable compartida (protegida por mutex)
    if (xSemaphoreTake(dataMutex, portMAX_DELAY)) {
      lastH = H;
      xSemaphoreGive(dataMutex);
    }
    
    // Medición cada 2 segundos
    vTaskDelay(2000 / portTICK_PERIOD_MS);
  }
}

// ---------- TAREA 2: Medición de Lluvia (hilo independiente) ----------
void rainTask(void *pvParameters) {
  (void) pvParameters;
  
  for (;;) {
    // Actualizar estado de lluvia
    updateRainState_raw();
    
    // Medición cada 100ms para detección rápida
    vTaskDelay(100 / portTICK_PERIOD_MS);
  }
}

// ---------- TAREA 3: Medición de IMU (Vibración + Inclinación) (hilo independiente) ----------
void imuTask(void *pvParameters) {
  (void) pvParameters;
  
  for (;;) {
    float peak = 0.0f;
    float rms  = readIMURMS_1s(peak);  // Toma ~1 segundo
    
    // Calcular Pitch/Roll desde acelerómetro
    xyzFloat aAng = mpu.getGValues();
    float ax = aAng.x, ay = aAng.y, az = aAng.z;
    float rollDeg  = atan2f(ay, az) * 180.0f / PI;
    float pitchDeg = atan2f(-ax, sqrtf(ay*ay + az*az)) * 180.0f / PI;
    
    // Actualizar variables compartidas (protegidas por mutex)
    if (xSemaphoreTake(dataMutex, portMAX_DELAY)) {
      lastRMS  = rms;
      lastPeak = peak;
      lastRoll  = lowpass(lastRoll,  rollDeg);
      lastPitch = lowpass(lastPitch, pitchDeg);
      xSemaphoreGive(dataMutex);
    }
    
    // Ya consumió ~1s, pequeño delay adicional
    vTaskDelay(50 / portTICK_PERIOD_MS);
  }
}

// ---------- TAREA 4: Cálculo de Score y Nivel de Alerta (hilo independiente) ----------
void scoreTask(void *pvParameters) {
  (void) pvParameters;
  
  for (;;) {
    float H, rms, peak;
    bool rRaw, rConf;
    
    // Leer variables compartidas (protegidas por mutex)
    if (xSemaphoreTake(dataMutex, portMAX_DELAY)) {
      H = lastH;
      rms = lastRMS;
      peak = lastPeak;
      rRaw = rainRaw;
      rConf = rainActive;
      xSemaphoreGive(dataMutex);
    }
    
    // Calcular ponderaciones
    float Sv = 0.0f;
    if (peak >= THR_V_PEAK || rms >= (rmsBase + 6.0f*rmsSigma)) Sv = 1.0f;
    else if (rms >= (rmsBase + 3.0f*rmsSigma)) Sv = 0.6f;

    float Sr = 0.0f;
    if (rConf) Sr = 0.5f;
    else if (rRaw) Sr = 0.2f;

    float Sh = 0.0f;
    if (H > 0.80f) Sh = 1.0f;
    else if (H > 0.60f) Sh = 0.6f;

    float Score = 0.45f*Sv + 0.35f*Sh + 0.20f*Sr;

    int level = 0; // 0=normal, 1=amarillo, 2=rojo
    if (Score >= 0.7f || (Sv == 1.0f && Sh >= 0.6f)) level = 2;
    else if (Score >= 0.4f) level = 1;
    
    // Actualizar variables compartidas
    if (xSemaphoreTake(dataMutex, portMAX_DELAY)) {
      lastScore = Score;
      lastLevel = level;
      xSemaphoreGive(dataMutex);
    }
    
    // Guardar en histórico
    Sample s;
    s.t = millis();
    s.H = H; s.rainRaw = rRaw; s.rainActive = rConf;
    s.rms = rms; s.peak = peak; s.Score = Score; s.level = level;
    history[histIndex] = s;
    histIndex = (histIndex + 1) % HISTORY_SIZE;
    if (histCount < HISTORY_SIZE) histCount++;
    
    // Log de depuración
    Serial.printf("[SCORE] H=%.0f%% rainRaw=%d conf=%d RMS=%.3f Pk=%.3f Score=%.2f Lvl=%d Pitch=%.1f Roll=%.1f\n",
                  H*100.0f, rRaw?1:0, rConf?1:0, rms, peak, Score, level, lastPitch, lastRoll);
    
    // Calcular cada 1.5 segundos
    vTaskDelay(1500 / portTICK_PERIOD_MS);
  }
}

// ---------- TAREA 5: Control de Actuadores (LED y Buzzer) (hilo independiente) ----------
void actuatorsTask(void *pvParameters) {
  (void) pvParameters;
  
  for (;;) {
    int level;
    bool silenced;
    
    // Leer variables compartidas
    if (xSemaphoreTake(dataMutex, portMAX_DELAY)) {
      level = lastLevel;
      silenced = alarmSilenced;
      xSemaphoreGive(dataMutex);
    }
    
    // Activar actuadores según nivel y estado de silenciado
    if (!silenced) {
      if (level == 2) {
        digitalWrite(PIN_LED, HIGH);
        tone(PIN_BUZZ, 2000);
      } else if (level == 1) {
        digitalWrite(PIN_LED, (millis()/300)%2);
        tone(PIN_BUZZ, 1200, 150);
      } else {
        digitalWrite(PIN_LED, LOW);
        noTone(PIN_BUZZ);
      }
    } else {
      digitalWrite(PIN_LED, LOW);
      noTone(PIN_BUZZ);
    }
    
    // Actualizar cada 200ms
    vTaskDelay(200 / portTICK_PERIOD_MS);
  }
}

// ---------- TAREA 6: Publicación MQTT al Gateway (hilo independiente) ----------
void mqttTask(void *pvParameters) {
  (void) pvParameters;
  
  for (;;) {
    if (mqttClient.connected()) {
      float H, rms, peak, score, pitch, roll;
      int level;
      bool rRaw, rConf, silenced;
      
      // Leer variables compartidas
      if (xSemaphoreTake(dataMutex, portMAX_DELAY)) {
        H = lastH;
        rms = lastRMS;
        peak = lastPeak;
        score = lastScore;
        level = lastLevel;
        pitch = lastPitch;
        roll = lastRoll;
        rRaw = rainRaw;
        rConf = rainActive;
        silenced = alarmSilenced;
        xSemaphoreGive(dataMutex);
      }
      
      // Construir JSON para enviar al Gateway
      String json = "{";
      json += "\"H\":" + String(H, 3) + ",";
      json += "\"rms\":" + String(rms, 4) + ",";
      json += "\"peak\":" + String(peak, 4) + ",";
      json += "\"Score\":" + String(score, 4) + ",";
      json += "\"level\":" + String(level) + ",";
      json += "\"pitch\":" + String(pitch, 1) + ",";
      json += "\"roll\":" + String(roll, 1) + ",";
      json += "\"rainRaw\":" + String(rRaw ? 1 : 0) + ",";
      json += "\"rainConf\":" + String(rConf ? 1 : 0) + ",";
      json += "\"silenced\":" + String(silenced ? 1 : 0);
      json += "}";
      
      // Publicar al broker MQTT
      if (mqttClient.publish(MQTT_TOPIC_SENSORS, json.c_str())) {
        Serial.printf("[MQTT] Datos enviados: Score=%.2f, Nivel=%d\n", score, level);
      } else {
        Serial.println("[MQTT] Error al publicar");
      }
    } else {
      Serial.println("[MQTT] Reconectando...");
      reconnectMQTT();
    }
    
    // Publicar cada 3 segundos
    vTaskDelay(3000 / portTICK_PERIOD_MS);
  }
}

// ====== Función de reconexión MQTT ======
void reconnectMQTT() {
  int attempts = 0;
  while (!mqttClient.connected() && attempts < 3) {
    Serial.print("[MQTT] Conectando al broker...");
    String clientId = "ESP32_Talud_" + String(random(0xffff), HEX);
    
    if (mqttClient.connect(clientId.c_str())) {
      Serial.println(" conectado!");
      // Suscribirse al topic de control
      mqttClient.subscribe(MQTT_TOPIC_CONTROL);
      Serial.printf("[MQTT] Suscrito a: %s\n", MQTT_TOPIC_CONTROL);
    } else {
      Serial.printf(" falló, rc=%d. Reintentando en 2s...\n", mqttClient.state());
      delay(2000);
      attempts++;
    }
  }
}

// ====== Callback MQTT (para recibir comandos del Gateway) ======
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  Serial.printf("[MQTT] Mensaje recibido en topic: %s\n", topic);
  
  String message = "";
  for (int i = 0; i < length; i++) {
    message += (char)payload[i];
  }
  Serial.printf("[MQTT] Payload: %s\n", message.c_str());
  
  // Comando de silenciar alarma
  if (String(topic) == MQTT_TOPIC_CONTROL) {
    if (message == "1") {
      alarmSilenced = !alarmSilenced;
      Serial.printf("[MQTT] Alarma silenciada: %s\n", alarmSilenced ? "SI" : "NO");
    }
  }
}

// ====== FIN DE TAREAS FREERTOS ======

// ====== Servidor Web (AP) ======
WebServer server(80);

// HTML + JS embebido (AÑADÍ Pitch y Roll en la tarjeta de KPIs)
const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Nodo SAT Talud</title>
<style>
  body{font-family:system-ui,-apple-system,Segoe UI,Roboto,Arial;margin:12px;background:#f7f7f7}
  .card{background:#fff;border-radius:10px;padding:14px;margin-bottom:12px;box-shadow:0 1px 4px rgba(0,0,0,.12)}
  h1{margin:0 0 8px;font-size:18px}
  .state{font-size:20px;font-weight:700}
  .small{color:#666;font-size:12px}
  .row{display:flex;gap:8px;flex-wrap:wrap}
  .kv{flex:1 1 140px;background:#fafafa;border:1px solid #eee;border-radius:8px;padding:10px}
  .btn{padding:10px 14px;border:0;border-radius:8px;cursor:pointer}
  .btn-red{background:#d32f2f;color:#fff}
  .btn-green{background:#2e7d32;color:#fff}
  pre{white-space:pre-wrap;font-size:12px}
</style>
</head>
<body>
  <h1>Nodo SAT - Talud</h1>
  <div class="card">
    <div id="state" class="state">Cargando...</div>
    <div id="leveldesc" class="small">—</div>
  </div>

  <div class="card">
    <div class="row">
      <div class="kv"><b>Humedad</b><br><span id="hum">--</span></div>
      <div class="kv"><b>Lluvia</b><br><span id="lluv">--</span></div>
      <div class="kv"><b>Vibración (RMS)</b><br><span id="rms">--</span> g</div>
      <div class="kv"><b>Pico</b><br><span id="peak">--</span> g</div>
      <div class="kv"><b>Score</b><br><span id="score">--</span></div>
      <div class="kv"><b>Silenciado</b><br><span id="sil">NO</span></div>
      <div class="kv"><b>Pitch</b><br><span id="pitch">--</span> °</div>
      <div class="kv"><b>Roll</b><br><span id="roll">--</span> °</div>
    </div>
  </div>

  <div class="card">
    <button id="toggleBtn" class="btn btn-red">Silenciar alarmas</button>
    <div class="small">El silenciado apaga LED y buzzer, pero las mediciones siguen.</div>
  </div>

  <div class="card">
    <b>Histórico reciente</b>
    <pre id="hist"></pre>
  </div>

<script>
async function fetchStatus(){
  const r = await fetch('/status'); const j = await r.json();
  document.getElementById('hum').innerText  = Math.round(j.H*100) + '%';
  document.getElementById('lluv').innerText = j.rainConf ? 'SI' : (j.rainRaw ? 'POSIBLE' : 'NO');
  document.getElementById('rms').innerText  = j.rms.toFixed(3);
  document.getElementById('peak').innerText = j.peak.toFixed(3);
  document.getElementById('score').innerText= j.Score.toFixed(2);
  document.getElementById('sil').innerText  = j.silenced ? 'SI' : 'NO';
  // NUEVO: Pitch/Roll
  document.getElementById('pitch').innerText = j.pitch.toFixed(1);
  document.getElementById('roll').innerText  = j.roll.toFixed(1);

  const state = document.getElementById('state');
  const desc  = document.getElementById('leveldesc');
  const btn   = document.getElementById('toggleBtn');

  if (j.level == 2){
    state.textContent = 'PELIGRO ALTO';
    desc.textContent  = 'Nivel 2 — Actuar inmediatamente';
    btn.className = 'btn btn-red';
    btn.textContent = j.silenced ? 'Reactivar alarmas' : 'Silenciar alarmas';
  } else if (j.level == 1){
    state.textContent = 'PRECAUCIÓN';
    desc.textContent  = 'Nivel 1 — Vigilar la zona';
    btn.className = 'btn btn-red';
    btn.textContent = j.silenced ? 'Reactivar alarmas' : 'Silenciar alarmas';
  } else {
    state.textContent = 'TODO TRANQUILO';
    desc.textContent  = 'Nivel 0 — Operación normal';
    btn.className = 'btn btn-green';
    btn.textContent = j.silenced ? 'Reactivar alarmas' : 'Silenciar alarmas';
  }
}

async function fetchHistory(){
  const r = await fetch('/history'); const arr = await r.json();
  let txt = '';
  for (let i=0;i<arr.length;i++){
    const s = arr[i];
    const ts = (s.t/1000).toFixed(0) + 's';
    const lluv = s.rainConf ? 'SI' : (s.rainRaw ? 'POS' : 'NO');
    txt += ts + ' | H:' + Math.round(s.H*100) + '%  L:' + lluv + 
           '  RMS:' + s.rms.toFixed(3) + '  Pk:' + s.peak.toFixed(2) + 
           '  Sc:' + s.Score.toFixed(2) + '  N:' + s.level + '\n';
  }
  document.getElementById('hist').textContent = txt;
}

document.getElementById('toggleBtn').addEventListener('click', async ()=>{
  const r = await fetch('/silence', {method:'POST'});
  const j = await r.json();
  await fetchStatus();
  await fetchHistory();
});

setInterval(()=>{ fetchStatus(); fetchHistory(); }, 2000);
window.onload = ()=>{ fetchStatus(); fetchHistory(); };
</script>
</body>
</html>
)HTML";

// ===== Endpoints JSON =====
String jsonStatus() {
  String s = "{";
  s += "\"H\":" + String(lastH,3) + ",";
  s += "\"rms\":" + String(lastRMS,4) + ",";
  s += "\"peak\":" + String(lastPeak,4) + ",";
  s += "\"Score\":" + String(lastScore,4) + ",";
  s += "\"level\":" + String(lastLevel) + ",";
  s += "\"rainRaw\":" + String(rainRaw?1:0) + ",";
  s += "\"rainConf\":" + String(rainActive?1:0) + ",";
  s += "\"silenced\":" + String(alarmSilenced?1:0) + ",";
  // ===== NUEVO: Pitch/Roll a la API
  s += "\"pitch\":" + String(lastPitch,1) + ",";
  s += "\"roll\":"  + String(lastRoll,1);
  s += "}";
  return s;
}

void handleRoot() { server.send_P(200, "text/html", INDEX_HTML); }
void handleStatus() { server.send(200, "application/json", jsonStatus()); }

void handleHistory() {
  String out = "[";
  int cnt = histCount;
  for (int i=0;i<cnt;i++){
    int idx = (histIndex - 1 - i + HISTORY_SIZE) % HISTORY_SIZE;
    Sample &s = history[idx];
    if (s.t == 0) continue;
    out += "{";
    out += "\"t\":" + String((unsigned long)s.t) + ",";
    out += "\"H\":" + String(s.H,3) + ",";
    out += "\"rms\":" + String(s.rms,4) + ",";
    out += "\"peak\":" + String(s.peak,4) + ",";
    out += "\"Score\":" + String(s.Score,4) + ",";
    out += "\"level\":" + String(s.level) + ",";
    out += "\"rainRaw\":" + String(s.rainRaw?1:0) + ",";
    out += "\"rainConf\":" + String(s.rainActive?1:0);
    out += "}";
    if (i < cnt-1) out += ",";
  }
  out += "]";
  server.send(200, "application/json", out);
}

void handleSilenceToggle() {
  alarmSilenced = !alarmSilenced;
  String j = "{\"silenced\":"+String(alarmSilenced?1:0)+"}";
  server.send(200, "application/json", j);
}

// ===== UI LCD (mensajes claros y banner en cambio de nivel) =====
void showLCD(float H, bool rainNow, bool rainConf, float score, int level){
  static uint32_t lastPageTs = 0;
  static uint8_t  page = 0;
  static int      lastLevelLCD = -1;
  static uint32_t levelChangeTs = 0;

  const int dwellMs  = 3000;  // alternancia pantallas
  const int bannerMs = 2000;  // banner al cambiar de nivel

  if (level != lastLevelLCD){
    lastLevelLCD = level;
    levelChangeTs = millis();
    lcd.clear();
    lcd.setCursor(0,0); lcd.print("CAMBIO DE NIVEL");
    lcd.setCursor(0,1);
    if (level == 2)      lcd.print("-> PELIGRO ALTO");
    else if (level == 1) lcd.print("-> PRECAUCION  ");
    else                 lcd.print("-> NORMAL      ");
    return;
  }
  if (millis() - levelChangeTs < (uint32_t)bannerMs) return;

  // de 2 pantallas a 3 pantallas (para inclinación)
  if (millis() - lastPageTs > (uint32_t)dwellMs) {
    page = (page + 1) % 3;
    lastPageTs = millis();
    lcd.clear();
  }

  int humPct  = (int)lround(H * 100.0f);
  int riskPct = (int)lround(score * 100.0f);
  bool llueve = (rainConf || rainNow);

  if (page == 0){
    lcd.setCursor(0,0);
    if (level == 2)      lcd.print("PELIGRO ALTO   ");
    else if (level == 1) lcd.print("PRECAUCION     ");
    else                 lcd.print("NORMAL         ");
    lcd.setCursor(0,1);
    char line2[17];
    snprintf(line2, sizeof(line2), "H:%2d%% Lluv:%s", humPct, llueve?"SI":"NO");
    lcd.print(line2);
  } 
  else if (page == 1) {
    lcd.setCursor(0,0);
    if (level == 0) lcd.print("Terreno Estable");
    else            lcd.print("Terreno Inestab");
    lcd.setCursor(0,1);
    char line2[17];
    snprintf(line2, sizeof(line2), "Riesgo:%2d%% N:%d", riskPct, level);
    lcd.print(line2);
  }
  else { // page == 2 (NUEVO)
    lcd.setCursor(0,0); lcd.print("Incl: Pitch Roll");
    lcd.setCursor(0,1);
    char line2[17];
    snprintf(line2, sizeof(line2), "%5.1f %5.1f  ", lastPitch, lastRoll);
    lcd.print(line2);
  }
}

// ====== SETUP ======
void setup() {
  Serial.begin(115200);
  delay(200);

  pinMode(PIN_LED, OUTPUT);
  pinMode(PIN_BUZZ, OUTPUT);
  pinMode(PIN_RAIN, INPUT_PULLUP);

  // Crear mutex para proteger datos compartidos
  dataMutex = xSemaphoreCreateMutex();
  if (dataMutex == NULL) {
    Serial.println("[ERROR] No se pudo crear el mutex");
    while(1);
  }

  // IMU en 19/18
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(100000);

  // LCD 16x2
  lcd.begin(16, 2);
  lcd.clear();
  lcd.setCursor(0,0); lcd.print("Inicializando...");
  lcd.setCursor(0,1); lcd.print("IMU/HUM/LLUVIA  ");

  // IMU init
  if (!mpu.init()) {
    Serial.println("[IMU] MPU6500 no encontrado.");
    lcd.clear();
    lcd.setCursor(0,0); lcd.print("IMU FAIL");
    lcd.setCursor(0,1); lcd.print("Revise cableado");
  } else {
    mpu.setAccRange(MPU6500_ACC_RANGE_4G);
    mpu.setGyrRange(MPU6500_GYRO_RANGE_500);
    Serial.println("[IMU] OK");
  }

  // Conectar a red WiFi existente (modo Station)
  WiFi.mode(WIFI_STA);
  Serial.printf("[WiFi] Conectando a red: %s\n", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  
  // Esperar conexión (timeout 20 segundos)
  int wifi_attempts = 0;
  while (WiFi.status() != WL_CONNECTED && wifi_attempts < 40) {
    delay(500);
    Serial.print(".");
    wifi_attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    IPAddress ip = WiFi.localIP();
    Serial.printf("\n[WiFi] Conectado! IP: %s\n", ip.toString().c_str());
    
    // Mostrar IP en LCD
    lcd.clear();
    lcd.setCursor(0,0); lcd.print("WiFi OK!");
    lcd.setCursor(0,1); lcd.print(ip.toString());
    delay(2000);
  } else {
    Serial.println("\n[WiFi] ERROR: No se pudo conectar a la red");
    lcd.clear();
    lcd.setCursor(0,0); lcd.print("WiFi ERROR");
    lcd.setCursor(0,1); lcd.print("Revise config");
    delay(5000);
  }

  // Configurar cliente MQTT
  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);
  
  // Intentar conectar al broker MQTT
  Serial.println("[MQTT] Conectando al broker...");
  reconnectMQTT();

  // HTTP server
  server.on("/", HTTP_GET, handleRoot);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/history", HTTP_GET, handleHistory);
  server.on("/silence", HTTP_POST, handleSilenceToggle);
  server.begin();
  Serial.println("HTTP server started.");

  // ===== CREAR TAREAS (HILOS) EN FREERTOS =====
  // Cumple con el requisito: "Medición desde hilos distintos al principal"
  
  // Tarea 1: Medición de Humedad (Core 0, Prioridad 2)
  xTaskCreatePinnedToCore(
    humidityTask,           // Función de la tarea
    "HumidityTask",         // Nombre
    4096,                   // Stack size
    NULL,                   // Parámetros
    2,                      // Prioridad
    &humidityTaskHandle,    // Handle
    0                       // Core 0
  );
  
  // Tarea 2: Medición de Lluvia (Core 0, Prioridad 2)
  xTaskCreatePinnedToCore(
    rainTask,
    "RainTask",
    4096,
    NULL,
    2,
    &rainTaskHandle,
    0
  );
  
  // Tarea 3: Medición de IMU (Core 1, Prioridad 3 - más alta por tiempo crítico)
  xTaskCreatePinnedToCore(
    imuTask,
    "IMUTask",
    8192,                   // Stack más grande para cálculos
    NULL,
    3,
    &imuTaskHandle,
    1                       // Core 1
  );
  
  // Tarea 4: Cálculo de Score (Core 1, Prioridad 2)
  xTaskCreatePinnedToCore(
    scoreTask,
    "ScoreTask",
    4096,
    NULL,
    2,
    &scoreTaskHandle,
    1
  );
  
  // Tarea 5: Control de Actuadores (Core 0, Prioridad 1)
  xTaskCreatePinnedToCore(
    actuatorsTask,
    "ActuatorsTask",
    4096,
    NULL,
    1,
    &actuatorsTaskHandle,
    0
  );
  
  // Tarea 6: Comunicación MQTT (Core 1, Prioridad 2)
  xTaskCreatePinnedToCore(
    mqttTask,
    "MQTTTask",
    8192,                   // Stack grande para network
    NULL,
    2,
    &mqttTaskHandle,
    1
  );

  Serial.println("=== Sistema listo (Arquitectura Multi-Hilo) ===");
  Serial.println("    Hilos creados:");
  Serial.println("    - HumidityTask (Core 0): Medición de humedad");
  Serial.println("    - RainTask (Core 0): Medición de lluvia");
  Serial.println("    - IMUTask (Core 1): Medición de vibración e inclinación");
  Serial.println("    - ScoreTask (Core 1): Cálculo de riesgo");
  Serial.println("    - ActuatorsTask (Core 0): Control LED/Buzzer");
  Serial.println("    - MQTTTask (Core 1): Comunicación con Gateway");
  Serial.println("=======================================================");
}

// ====== LOOP (ligero: servidor + LCD + MQTT) ======
void loop() {
  // Mantener conexión MQTT
  if (!mqttClient.connected()) {
    reconnectMQTT();
  }
  mqttClient.loop();
  
  // Atender peticiones del servidor web
  server.handleClient();

  // Refresca LCD con últimos valores (no bloquea)
  // Usar variables protegidas por mutex
  float H, score;
  int level;
  bool rRaw, rConf;
  
  if (xSemaphoreTake(dataMutex, 10 / portTICK_PERIOD_MS)) {
    H = lastH;
    score = lastScore;
    level = lastLevel;
    rRaw = rainRaw;
    rConf = rainActive;
    xSemaphoreGive(dataMutex);
    
    showLCD(H, rRaw, rConf, score, level);
  }

  delay(10);
}