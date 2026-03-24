/*
 * ╔═══════════════════════════════════════════════════════════════╗
 * ║           GUBER-LAB  —  Firmware ESP32 v2.2                  ║
 * ║  8 Relés · RGB LED · Buzzer · WiFi Manager · MQTT            ║
 * ║  Clima via API (IP → lat/lon → Open-Meteo) · EmailJS · NTP   ║
 * ╚═══════════════════════════════════════════════════════════════╝
 *
 * PINOS:
 *   GPIO 4   → Botão (toque rápido = troca canal, longo = toggle relé)
 *   GPIO 2   → Buzzer
 *   GPIO 13  → LED RGB — Vermelho (R)  [ânodo comum]
 *   GPIO 12  → LED RGB — Verde    (G)  [ânodo comum]
 *   GPIO 14  → LED RGB — Azul     (B)  [ânodo comum]
 *   GPIO 16..23, 25 → Relés 1-8  (ajuste RELAY_PINS[] se necessário)
 *
 * DEPENDÊNCIAS (instale pelo Library Manager):
 *   - PubSubClient      (Nick O'Leary)
 *   - ArduinoJson       (Benoit Blanchon) v6+
 *   (WiFi, HTTPClient, time.h já vêm com o ESP32 core — sem libs extras)
 *
 * WiFi padrão:   configure WIFI_SSID / WIFI_PASS abaixo (ou use o portal AP)
 * Portal AP:     GuberLab-Config / admin / admin123
 *
 * FLUXO DE CLIMA:
 *   1. ipapi.co/json          → descobre lat/lon pela rede WiFi
 *   2. api.open-meteo.com     → busca temperatura e umidade atuais
 *   3. Publica TEMP:xx,HUM:xx no tópico MQTT de status
 *
 * PROTOCOLO MQTT:
 *   Tópico de comandos:  guberlab/comando
 *   Tópico de status:    guberlab/status
 *
 *   Comandos recebidos:
 *     PING              → responde PONG
 *     STATUS            → publica estado de todos os relés + clima
 *     T1-T8             → toggle do canal
 *     L1-L8             → liga canal
 *     D1-D8             → desliga canal
 *     L_TUDO            → liga todos
 *     D_TUDO            → desliga todos
 *     SONG_BEEP         → toca bip simples
 *     SONG_MARIO        → toca tema do Mario
 *     SONG_ALARM        → toca alarme
 *     SONG_VICTORY      → toca fanfarra de vitória
 *     DASHBOARD_ONLINE  → LED pisca e publica clima
 *     SEND_EMAIL:<msg>  → dispara e-mail via EmailJS
 *
 *   Publicações periódicas (a cada 10 min):
 *     TEMP:25.3,HUM:68.2
 */

// ─────────────────────────────────────────────
//  BIBLIOTECAS
// ─────────────────────────────────────────────
#include <WiFi.h>
#include <HTTPClient.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <WebServer.h>
#include <time.h>           // NTP — já incluso no ESP32 core

// ─────────────────────────────────────────────
//  PINOS
// ─────────────────────────────────────────────
#define PIN_BUTTON   4
#define PIN_BUZZER   2
#define PIN_LED_R    13
#define PIN_LED_G    12
#define PIN_LED_B    14

// 8 relés — ajuste conforme sua fiação
const int RELAY_PINS[8] = {16, 17, 18, 19, 21, 22, 23, 25};

// ─────────────────────────────────────────────
//  APIs DE CLIMA (sem chave, gratuitas)
// ─────────────────────────────────────────────
// Etapa 1 — localização pelo IP da rede WiFi
#define GEO_API_URL  "http://ip-api.com/json/?fields=lat,lon,city"
// Etapa 2 — clima atual pela coordenada
// (temperatura 2m + umidade relativa 2m + atual)
#define WEATHER_API  "https://api.open-meteo.com/v1/forecast?" \
                     "latitude=%s&longitude=%s" \
                     "&current=temperature_2m,relative_humidity_2m" \
                     "&timezone=America%2FSao_Paulo"

// ─────────────────────────────────────────────
//  MQTT
// ─────────────────────────────────────────────
#define MQTT_HOST    "broker.hivemq.com"
#define MQTT_PORT    1883
#define TOPIC_CMD    "guberlab/comando"
#define TOPIC_STATUS "guberlab/status"

// ─────────────────────────────────────────────
//  WiFi padrão
// ─────────────────────────────────────────────
// ── WIFI — altere para sua rede (ou deixe em branco: o portal de configuração cuida disso) ──
#define WIFI_SSID    "NOME_DA_SUA_REDE"
#define WIFI_PASS    "SENHA_DA_SUA_REDE"

// ─────────────────────────────────────────────
//  PORTAL DE CONFIGURAÇÃO
// ─────────────────────────────────────────────
#define PORTAL_SSID  "GuberLab-Config"
#define PORTAL_USER  "admin"
#define PORTAL_PASS  "admin123"

// ─────────────────────────────────────────────
//  EMAILJS — mesmos IDs do dashboard HTML
// ─────────────────────────────────────────────
// ── EMAILJS — copie os mesmos valores que você colocou no index.html ──────────
#define EMAILJS_SERVICE  "SEU_SERVICE_ID"    // ex: "service_abc123"
#define EMAILJS_TEMPLATE "SEU_TEMPLATE_ID"   // ex: "template_xyz789"
#define EMAILJS_KEY      "SUA_PUBLIC_KEY"    // em Account → General
#define EMAILJS_TO       "seu@email.com"     // destinatário dos relatórios

// ─────────────────────────────────────────────
//  NTP — horário real via internet
// ─────────────────────────────────────────────
#define NTP_SERVER   "pool.ntp.org"
#define NTP_OFFSET   -10800   // UTC-3 (Brasília) em segundos
#define NTP_INTERVAL 3600     // re-sincroniza a cada 1h

// ─────────────────────────────────────────────
//  ESTADO GLOBAL
// ─────────────────────────────────────────────
bool  relayState[8]   = {false};
int   selectedChannel = 0;
bool  wifiConnected   = false;
bool  mqttConnected   = false;

// Botão
unsigned long btnPressTime  = 0;
#define LONG_PRESS_MS  600
#define DEBOUNCE_MS     50

// LED / Buzzer
unsigned long ledTimer      = 0;
bool          ledBlinkState = false;

// MQTT reconexão
unsigned long lastMqttRetry = 0;
#define MQTT_RETRY_MS   5000

// Clima — atualização periódica
unsigned long lastWeatherFetch = 0;
#define WEATHER_INTERVAL_MS  600000  // busca clima a cada 10 minutos
float lastTemp = NAN;
float lastHum  = NAN;
char  lastCity[32] = "";

// Configurações salvas
Preferences prefs;
char  cfgSSID[64] = WIFI_SSID;
char  cfgPASS[64] = WIFI_PASS;

WiFiClient   wifiClient;
PubSubClient mqtt(wifiClient);
WebServer    portalServer(80);

// ─────────────────────────────────────────────
//  LED RGB
// ─────────────────────────────────────────────
// LED ÂNODO COMUM: perna comum no positivo (VCC).
// Cada cor recebe o negativo — lógica INVERTIDA: 0 = máximo brilho, 255 = apagado.
void setRGB(int r, int g, int b) {
  analogWrite(PIN_LED_R, 255 - r);
  analogWrite(PIN_LED_G, 255 - g);
  analogWrite(PIN_LED_B, 255 - b);
}
void ledOff()      { setRGB(0,   0,   0);   }
void ledOrange()   { setRGB(255, 60,  0);   }
void ledOrangeDim(){ setRGB(100, 24,  0);   }
void ledBlue()     { setRGB(0,   0,   153); }
void ledRed()      { setRGB(200, 0,   0);   }
void ledGreen()    { setRGB(0,   200, 0);   }
void ledCyan()     { setRGB(0,   180, 180); }
void ledMagenta()  { setRGB(180, 0,   180); }
void ledYellow()   { setRGB(200, 180, 0);   }
void ledWhite()    { setRGB(180, 180, 180); }

void applyChannelColor() {
  if (relayState[selectedChannel]) ledGreen();
  else                             ledRed();
}

// ─────────────────────────────────────────────
//  BUZZER — notas
// ─────────────────────────────────────────────
void beepSimple(int freq, int dur) {
  tone(PIN_BUZZER, freq, dur);
  delay(dur + 10);
  noTone(PIN_BUZZER);
}

void melodyConnecting() {
  beepSimple(880, 80); delay(60);
  beepSimple(880, 80);
}

void melodyConnected() {
  beepSimple(523, 80); delay(40);
  beepSimple(659, 80); delay(40);
  beepSimple(784, 120);
}

void beepChannelChange() { beepSimple(1200, 50); }

void beepRelayOn() {
  beepSimple(880, 60); delay(30);
  beepSimple(1100, 80);
}

void beepRelayOff() {
  beepSimple(600, 60); delay(30);
  beepSimple(400, 80);
}

// ─────────────────────────────────────────────
//  MÚSICAS (Easter Egg via Dashboard)
// ─────────────────────────────────────────────

// Bip simples de confirmação
void songBeep() {
  beepSimple(1000, 80); delay(40);
  beepSimple(1400, 120);
}

// Super Mario Bros — tema principal (simplificado)
void songMario() {
  // Frequências: E5=659, C5=523, G4=392, A4=440, B4=494, D5=587, F5=698, G5=784
  int n[] = {659,659,0,659,0,523,659,0,784,0,392,0,
             523,0,392,0,330,0,440,494,466,440,0,
             392,659,784,880,698,784,0,659,523,587,494};
  int d[] = {150,150,150,150,150,150,150,150,200,400,200,400,
             200,400,200,400,200,300,150,150,150,200,150,
             130,130,130,150,150,150,150,130,130,130,200};
  for (int i = 0; i < 35; i++) {
    if (n[i] == 0) noTone(PIN_BUZZER);
    else           tone(PIN_BUZZER, n[i]);
    delay(d[i]);
  }
  noTone(PIN_BUZZER);
}

// Alarme pulsante
void songAlarm() {
  // ~10s: cada ciclo sweep up+down ~0.8s × 12 repetições
  for (int rep = 0; rep < 12; rep++) {
    for (int f = 800; f <= 1800; f += 50) {
      tone(PIN_BUZZER, f); delay(10);
    }
    for (int f = 1800; f >= 800; f -= 50) {
      tone(PIN_BUZZER, f); delay(10);
    }
  }
  noTone(PIN_BUZZER);
}

// Fanfarra de vitória
void songVictory() {
  int n[] = {523,523,523,415,622,523,415,622,523,
             784,784,784,830,622,523,415,622,523};
  int d[] = {120,120,120,90, 30, 120,90, 30, 360,
             120,120,120,90, 30, 120,90, 30, 360};
  for (int i = 0; i < 18; i++) {
    tone(PIN_BUZZER, n[i]);
    delay(d[i]);
    noTone(PIN_BUZZER);
    delay(15);
  }
}

// Toca a música e sincroniza LED colorido
void playSong(const char* key) {
  Serial.printf("[Song] Tocando: %s\n", key);
  if (strcmp(key, "SONG_BEEP") == 0) {
    ledCyan(); songBeep(); applyChannelColor();

  } else if (strcmp(key, "SONG_MARIO") == 0) {
    // LED dança colorido durante Mario
    // (a música é longa — LED apenas começa laranja)
    ledOrange(); songMario(); applyChannelColor();

  } else if (strcmp(key, "SONG_ALARM") == 0) {
    // LED vermelho piscando durante alarme
    songAlarm(); applyChannelColor();

  } else if (strcmp(key, "SONG_VICTORY") == 0) {
    // LED arco-íris rápido durante vitória
    for (int i = 0; i < 3; i++) {
      ledYellow(); delay(60); ledCyan(); delay(60);
      ledMagenta(); delay(60); ledGreen(); delay(60);
    }
    songVictory(); applyChannelColor();
  }
}

// ─────────────────────────────────────────────
//  RELÉS
// ─────────────────────────────────────────────
void initRelays() {
  for (int i = 0; i < 8; i++) {
    pinMode(RELAY_PINS[i], OUTPUT);
    digitalWrite(RELAY_PINS[i], HIGH); // módulo ativo LOW → HIGH = desligado
  }
}

void setRelay(int ch, bool state) {
  if (ch < 0 || ch > 7) return;
  relayState[ch] = state;
  digitalWrite(RELAY_PINS[ch], state ? LOW : HIGH);
}

void toggleRelay(int ch) { setRelay(ch, !relayState[ch]); }

void publishState(int ch) {
  char buf[16];
  snprintf(buf, sizeof(buf), "S%d:%d", ch + 1, relayState[ch] ? 1 : 0);
  mqtt.publish(TOPIC_STATUS, buf);
}

void publishAllStates() {
  for (int i = 0; i < 8; i++) {
    publishState(i);
    delay(20);
  }
}

// ─────────────────────────────────────────────
//  CLIMA — busca por IP → lat/lon → Open-Meteo
// ─────────────────────────────────────────────

// Etapa 1: descobre lat/lon pelo IP da rede
bool fetchGeoLocation(float &lat, float &lon) {
  HTTPClient http;
  http.begin(GEO_API_URL);
  http.setTimeout(8000);
  int code = http.GET();
  if (code != 200) {
    Serial.printf("[Geo] Falha HTTP %d\n", code);
    http.end(); return false;
  }

  StaticJsonDocument<128> doc;
  DeserializationError err = deserializeJson(doc, http.getString());
  http.end();
  if (err) { Serial.println("[Geo] JSON inválido"); return false; }

  lat = doc["lat"] | 0.0f;
  lon = doc["lon"] | 0.0f;
  const char* city = doc["city"] | "";
  strncpy(lastCity, city, sizeof(lastCity) - 1);

  Serial.printf("[Geo] Cidade: %s  lat=%.4f  lon=%.4f\n", lastCity, lat, lon);
  return (lat != 0.0f || lon != 0.0f);
}

// Etapa 2: busca temperatura e umidade pela coordenada
bool fetchWeather(float lat, float lon) {
  char url[256];
  char latStr[12], lonStr[12];
  dtostrf(lat, 1, 4, latStr);
  dtostrf(lon, 1, 4, lonStr);
  snprintf(url, sizeof(url), WEATHER_API, latStr, lonStr);

  HTTPClient http;
  http.begin(url);
  http.setTimeout(8000);
  int code = http.GET();
  if (code != 200) {
    Serial.printf("[Weather] Falha HTTP %d\n", code);
    http.end(); return false;
  }

  // Resposta é grande — parseia em stream para economizar RAM
  StaticJsonDocument<512> doc;
  DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();
  if (err) { Serial.println("[Weather] JSON inválido"); return false; }

  lastTemp = doc["current"]["temperature_2m"]       | NAN;
  lastHum  = doc["current"]["relative_humidity_2m"] | NAN;

  Serial.printf("[Weather] Temp=%.1f°C  Hum=%.1f%%\n", lastTemp, lastHum);
  return (!isnan(lastTemp) && !isnan(lastHum));
}

// Publica TEMP:xx.x,HUM:xx.x no MQTT
void publishSensor() {
  if (WiFi.status() != WL_CONNECTED) return;

  float lat, lon;
  // Só rebusca geolocalização se ainda não temos (ou a cada reinicio)
  static float cachedLat = 0, cachedLon = 0;
  static bool  geoOk     = false;

  if (!geoOk) {
    geoOk = fetchGeoLocation(lat, lon);
    if (geoOk) { cachedLat = lat; cachedLon = lon; }
  } else {
    lat = cachedLat; lon = cachedLon;
  }

  if (!geoOk) {
    Serial.println("[Sensor] Sem geolocalização — clima indisponível");
    return;
  }

  if (!fetchWeather(lat, lon)) return;

  char buf[40];
  snprintf(buf, sizeof(buf), "TEMP:%.1f,HUM:%.1f", lastTemp, lastHum);
  mqtt.publish(TOPIC_STATUS, buf);
  Serial.printf("[Sensor] Publicado: %s\n", buf);
}

// ─────────────────────────────────────────────
//  MQTT — callback de mensagens recebidas
// ─────────────────────────────────────────────
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  char msg[256] = {0};
  for (unsigned int i = 0; i < length && i < 255; i++) msg[i] = (char)payload[i];

  Serial.print("[MQTT] ← "); Serial.println(msg);

  // ── PING ──
  if (strcmp(msg, "PING") == 0) {
    mqtt.publish(TOPIC_STATUS, "PONG");
    return;
  }

  // ── STATUS ──
  if (strcmp(msg, "STATUS") == 0) {
    publishAllStates();
    publishSensor();
    return;
  }

  // ── DASHBOARD ONLINE ── (dashboard acabou de conectar)
  if (strcmp(msg, "DASHBOARD_ONLINE") == 0) {
    // Pisca LED azul 2x para indicar reconhecimento
    for (int i = 0; i < 2; i++) {
      ledBlue(); delay(120);
      ledOff();  delay(80);
    }
    applyChannelColor();
    publishAllStates();
    publishSensor();  // manda temperatura/umidade imediatamente
    return;
  }

  // ── LIGAR / DESLIGAR TUDO ──
  if (strcmp(msg, "L_TUDO") == 0) {
    for (int i = 0; i < 8; i++) setRelay(i, true);
    publishAllStates();
    applyChannelColor();
    beepRelayOn();
    return;
  }
  if (strcmp(msg, "D_TUDO") == 0) {
    for (int i = 0; i < 8; i++) setRelay(i, false);
    publishAllStates();
    applyChannelColor();
    beepRelayOff();
    return;
  }

  // ── MÚSICAS (Easter Egg) ──
  if (strncmp(msg, "SONG_", 5) == 0) {
    playSong(msg);
    return;
  }

  // ── SEND_EMAIL:<mensagem> — disparo manual pelo dashboard ──
  // Exemplo: "SEND_EMAIL:Relé 1 ativado manualmente"
  if (strncmp(msg, "SEND_EMAIL:", 11) == 0) {
    String payload = String(msg + 11);   // tudo após "SEND_EMAIL:"
    if (payload.length() == 0) payload = "Relatório solicitado pelo dashboard.";

    // Monta log completo com estado dos relés e sensor
    String log = payload + "\n\n--- STATUS ---\n";
    log += "Horário: " + getNtpTimestamp() + "\n";
    if (strlen(lastCity) > 0) log += "Localização: " + String(lastCity) + "\n";
    if (!isnan(lastTemp)) log += "Temperatura: " + String(lastTemp, 1) + "°C\n";
    if (!isnan(lastHum))  log += "Umidade: "     + String(lastHum,  1) + "%\n";
    log += "\nEstado dos relés:\n";
    for (int i = 0; i < 8; i++) {
      log += "  Canal " + String(i + 1) + ": " + (relayState[i] ? "LIGADO" : "DESLIGADO") + "\n";
    }

    enviarRelatorioEmail(log);
    mqtt.publish(TOPIC_STATUS, "EMAIL_SENT");
    return;
  }

  // ── T1-T8  (toggle) ──
  if (msg[0] == 'T' && length == 2) {
    int ch = msg[1] - '1';
    if (ch >= 0 && ch < 8) {
      toggleRelay(ch);
      publishState(ch);
      if (ch == selectedChannel) applyChannelColor();
      if (relayState[ch]) beepRelayOn(); else beepRelayOff();
    }
    return;
  }

  // ── L1-L8  (liga) ──
  if (msg[0] == 'L' && length == 2) {
    int ch = msg[1] - '1';
    if (ch >= 0 && ch < 8) {
      setRelay(ch, true);
      publishState(ch);
      if (ch == selectedChannel) applyChannelColor();
      beepRelayOn();
    }
    return;
  }

  // ── D1-D8  (desliga) ──
  if (msg[0] == 'D' && length == 2) {
    int ch = msg[1] - '1';
    if (ch >= 0 && ch < 8) {
      setRelay(ch, false);
      publishState(ch);
      if (ch == selectedChannel) applyChannelColor();
      beepRelayOff();
    }
    return;
  }
}

// ─────────────────────────────────────────────
//  MQTT — conectar / reconectar
// ─────────────────────────────────────────────
void mqttConnect() {
  if (!wifiConnected) return;
  if (mqtt.connected()) return;

  char clientId[32];
  snprintf(clientId, sizeof(clientId), "guberESP_%04X", (uint16_t)(ESP.getEfuseMac()));

  Serial.printf("[MQTT] Conectando como '%s'... ", clientId);

  if (mqtt.connect(clientId)) {
    Serial.println("OK");
    mqttConnected = true;
    mqtt.subscribe(TOPIC_CMD);
    Serial.printf("[MQTT] Inscrito em '%s'\n", TOPIC_CMD);
    publishAllStates();
    publishSensor();        // publica sensor logo ao conectar
    ledBlue();
  } else {
    Serial.printf("falhou, rc=%d\n", mqtt.state());
    mqttConnected = false;
  }
}

// ─────────────────────────────────────────────
//  PORTAL WEB DE CONFIGURAÇÃO
// ─────────────────────────────────────────────
bool checkAuth() {
  if (!portalServer.authenticate(PORTAL_USER, PORTAL_PASS)) {
    portalServer.requestAuthentication();
    return false;
  }
  return true;
}

void handlePortalRoot() {
  if (!checkAuth()) return;

  String html = R"rawHTML(
<!DOCTYPE html><html lang="pt-br">
<head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>GuberLab Config</title>
<style>
  body{font-family:'Courier New',monospace;background:#080808;color:#e0e0e0;margin:0;padding:20px}
  h1{color:#ff6a00;letter-spacing:6px;font-size:1.5rem}
  .card{background:#131313;border:1px solid #222;border-radius:12px;padding:24px;max-width:460px;margin:auto}
  label{display:block;font-size:11px;letter-spacing:2px;color:#666;margin-bottom:6px;margin-top:16px}
  input{width:100%;padding:10px 14px;background:#0a0a0a;border:1px solid #2a2a2a;border-radius:6px;
        color:#e0e0e0;font-family:inherit;font-size:14px;box-sizing:border-box}
  input:focus{outline:none;border-color:#ff6a00}
  button{width:100%;padding:13px;background:#ff6a00;color:#000;font-family:inherit;font-weight:700;
         font-size:12px;letter-spacing:3px;border:none;border-radius:6px;cursor:pointer;margin-top:20px}
  button:hover{background:#ff8c3a}
  .hint{font-size:11px;color:#444;text-align:center;margin-top:16px}
  .status{padding:10px;border-radius:6px;margin-top:12px;font-size:12px;letter-spacing:1px;text-align:center}
  .ok{background:rgba(0,230,118,0.1);color:#00e676;border:1px solid rgba(0,230,118,0.2)}
  .err{background:rgba(244,67,54,0.1);color:#f44336;border:1px solid rgba(244,67,54,0.2)}
  .sensor-box{display:flex;gap:16px;margin-top:12px}
  .sensor-val{flex:1;background:#0a0a0a;border:1px solid #222;border-radius:8px;padding:12px;text-align:center}
  .sensor-val span{display:block;font-size:1.4rem;color:#ff6a00;font-weight:700}
  .sensor-val small{font-size:10px;color:#444;letter-spacing:2px}
</style>
</head>
<body>
<div class="card">
  <h1>GUBER-LAB</h1>
  <p style="color:#666;font-size:12px;letter-spacing:2px">CONFIGURAÇÃO DO SISTEMA v2.1</p>
)rawHTML";

  html += "<div class='status ";
  html += wifiConnected ? "ok'>● WIFI CONECTADO — " : "err'>✕ SEM CONEXÃO WIFI — ";
  html += String(cfgSSID) + "</div>";

  // Mostra leitura do sensor no portal
  html += "<div class='sensor-box'>";
  html += "<div class='sensor-val'><span>";
  html += (!isnan(lastTemp)) ? String(lastTemp, 1) + "°C" : "—";
  html += "</span><small>TEMPERATURA</small></div>";
  html += "<div class='sensor-val'><span>";
  html += (!isnan(lastHum)) ? String(lastHum, 1) + "%" : "—";
  html += "</span><small>UMIDADE</small></div>";
  html += "</div>";
  if (strlen(lastCity) > 0) {
    html += "<p style='font-size:10px;color:#444;text-align:center;margin-top:8px;letter-spacing:1px'>";
    html += "📍 Clima de: ";
    html += String(lastCity);
    html += "</p>";
  }

  html += R"rawHTML(
  <form action="/save" method="POST">
    <label>NOME DA REDE (SSID)</label>
    <input name="ssid" maxlength="63" value=")rawHTML";
  html += String(cfgSSID);
  html += R"rawHTML(">
    <label>SENHA DA REDE</label>
    <input name="pass" type="password" maxlength="63">
    <button type="submit">SALVAR E REINICIAR</button>
  </form>
  <div class="hint">Após salvar o ESP32 reiniciará e tentará conectar.</div>
  <br>
  <form action="/reset" method="POST">
    <button type="submit" style="background:#1a1a1a;color:#f44336;border:1px solid #2a0000">
      ⚠ RESETAR PARA PADRÃO
    </button>
  </form>
</div>
</body></html>
)rawHTML";

  portalServer.send(200, "text/html", html);
}

void handlePortalSave() {
  if (!checkAuth()) return;
  if (portalServer.hasArg("ssid")) {
    portalServer.arg("ssid").toCharArray(cfgSSID, 64);
  }
  if (portalServer.hasArg("pass") && portalServer.arg("pass").length() > 0) {
    portalServer.arg("pass").toCharArray(cfgPASS, 64);
  }
  prefs.begin("guberlab", false);
  prefs.putString("ssid", cfgSSID);
  prefs.putString("pass", cfgPASS);
  prefs.end();

  portalServer.send(200, "text/html",
    "<html><body style='background:#080808;color:#00e676;font-family:monospace;"
    "text-align:center;padding:40px'><h2>CONFIGURAÇÃO SALVA</h2>"
    "<p>Reiniciando em 2 segundos...</p></body></html>");
  delay(2000);
  ESP.restart();
}

void handlePortalReset() {
  if (!checkAuth()) return;
  strncpy(cfgSSID, WIFI_SSID, 64);
  strncpy(cfgPASS, WIFI_PASS, 64);
  prefs.begin("guberlab", false);
  prefs.remove("ssid");
  prefs.remove("pass");
  prefs.end();

  portalServer.send(200, "text/html",
    "<html><body style='background:#080808;color:#ff6a00;font-family:monospace;"
    "text-align:center;padding:40px'><h2>RESETADO</h2><p>Reiniciando...</p></body></html>");
  delay(1500);
  ESP.restart();
}

// ─────────────────────────────────────────────
//  NTP — retorna timestamp formatado
// ─────────────────────────────────────────────
String getNtpTimestamp() {
  struct tm ti;
  if (!getLocalTime(&ti)) return "sem horário";
  char buf[32];
  strftime(buf, sizeof(buf), "%d/%m/%Y, %H:%M:%S", &ti);
  return String(buf);
}

// ─────────────────────────────────────────────
//  EMAILJS — envio de relatório
// ─────────────────────────────────────────────
void enviarRelatorioEmail(String mensagemLog) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[Email] Sem WiFi — e-mail cancelado");
    return;
  }

  Serial.println("[Email] Enviando via EmailJS...");

  HTTPClient http;
  http.begin("https://api.emailjs.com/api/v1.0/email/send");
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(10000);   // 10s de timeout

  StaticJsonDocument<512> doc;
  doc["service_id"]  = EMAILJS_SERVICE;
  doc["template_id"] = EMAILJS_TEMPLATE;
  doc["user_id"]     = EMAILJS_KEY;

  JsonObject params = doc.createNestedObject("template_params");
  params["to_email"]   = EMAILJS_TO;
  params["subject"]    = "GUBER-LAB | Relatório ESP32";
  params["timestamp"]  = getNtpTimestamp();
  params["mensagem"]   = mensagemLog;

  String body;
  serializeJson(doc, body);

  int code = http.POST(body);
  if (code == 200) {
    Serial.println("[Email] ✓ Enviado com sucesso!");
    ledGreen(); delay(300); applyChannelColor();
  } else {
    Serial.printf("[Email] ✗ Falhou — código HTTP: %d\n", code);
    ledRed(); delay(300); applyChannelColor();
  }
  http.end();
}

void startPortal() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(PORTAL_SSID);
  Serial.print("[Portal] IP: ");
  Serial.println(WiFi.softAPIP());

  portalServer.on("/",      HTTP_GET,  handlePortalRoot);
  portalServer.on("/save",  HTTP_POST, handlePortalSave);
  portalServer.on("/reset", HTTP_POST, handlePortalReset);
  portalServer.begin();
  Serial.println("[Portal] Servidor iniciado — conecte ao AP 'GuberLab-Config'");
}

// ─────────────────────────────────────────────
//  WiFi — conectar
// ─────────────────────────────────────────────
bool connectWiFi() {
  Serial.printf("[WiFi] Conectando a '%s'...\n", cfgSSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(cfgSSID, cfgPASS);

  unsigned long t = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - t > 15000) return false;
    delay(100);
  }
  Serial.print("[WiFi] Conectado! IP: ");
  Serial.println(WiFi.localIP());
  return true;
}

// ─────────────────────────────────────────────
//  SETUP
// ─────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  Serial.println("\n\n╔══════════════════════════╗");
  Serial.println("║    GUBER-LAB  v2.1       ║");
  Serial.println("╚══════════════════════════╝");

  // --- Pinos ---
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  pinMode(PIN_LED_R, OUTPUT);
  pinMode(PIN_LED_G, OUTPUT);
  pinMode(PIN_LED_B, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);

  // --- Relés ---
  initRelays();

  // --- Carregar configs salvas ---
  prefs.begin("guberlab", true);
  String savedSSID = prefs.getString("ssid", "");
  String savedPASS = prefs.getString("pass", "");
  prefs.end();
  if (savedSSID.length() > 0) {
    savedSSID.toCharArray(cfgSSID, 64);
    savedPASS.toCharArray(cfgPASS, 64);
  }

  // --- LED piscando laranja + música enquanto conecta ---
  melodyConnecting();
  ledOrange();

  // --- Tentar WiFi ---
  wifiConnected = connectWiFi();

  if (wifiConnected) {
    ledBlue();
    melodyConnected();
    Serial.println("[WiFi] Conectado!");

    // NTP — sincroniza horário
    configTime(NTP_OFFSET, 0, NTP_SERVER);
    Serial.print("[NTP] Sincronizando horário");
    struct tm ti;
    int tries = 0;
    while (!getLocalTime(&ti) && tries < 20) {
      Serial.print(".");
      delay(500);
      tries++;
    }
    if (getLocalTime(&ti)) {
      char buf[32];
      strftime(buf, sizeof(buf), "%d/%m/%Y %H:%M:%S", &ti);
      Serial.printf("\n[NTP] Hora: %s\n", buf);
    } else {
      Serial.println("\n[NTP] Falha na sincronização (sem internet?)");
    }

    // MQTT
    mqtt.setServer(MQTT_HOST, MQTT_PORT);
    mqtt.setCallback(mqttCallback);
    mqtt.setKeepAlive(30);
    mqtt.setBufferSize(512);   // buffer maior para SEND_EMAIL e sensor
    mqttConnect();

  } else {
    Serial.println("[WiFi] Falha — abrindo portal AP...");
    startPortal();
  }
}

// ─────────────────────────────────────────────
//  LOOP
// ─────────────────────────────────────────────
void loop() {
  // --- Modo portal AP ---
  if (!wifiConnected) {
    portalServer.handleClient();
    if (millis() - ledTimer > 800) {
      ledTimer = millis();
      ledBlinkState = !ledBlinkState;
      if (ledBlinkState) ledOrange(); else ledOff();
    }
    handleButton();
    return;
  }

  // --- Piscar laranja enquanto MQTT não conectou ---
  if (!mqttConnected) {
    if (millis() - ledTimer > 500) {
      ledTimer = millis();
      ledBlinkState = !ledBlinkState;
      if (ledBlinkState) ledOrange(); else ledOff();
    }
  }

  // --- MQTT loop e reconexão ---
  if (!mqtt.connected()) {
    mqttConnected = false;
    if (millis() - lastMqttRetry > MQTT_RETRY_MS) {
      lastMqttRetry = millis();
      mqttConnect();
    }
  } else {
    mqttConnected = true;
    mqtt.loop();
  }

  // --- Publicação periódica do clima ---
  if (mqttConnected && (millis() - lastWeatherFetch > WEATHER_INTERVAL_MS)) {
    lastWeatherFetch = millis();
    publishSensor();
  }

  // --- Botão ---
  handleButton();
}

// ─────────────────────────────────────────────
//  LEITURA DO BOTÃO
// ─────────────────────────────────────────────
void handleButton() {
  static unsigned long lastDebounce = 0;
  static bool          lastRaw      = HIGH;
  static bool          pressed      = false;

  bool raw = digitalRead(PIN_BUTTON);

  if (raw != lastRaw) {
    lastDebounce = millis();
    lastRaw = raw;
  }
  if (millis() - lastDebounce < DEBOUNCE_MS) return;

  bool isDown = (raw == LOW);

  if (isDown && !pressed) {
    pressed = true;
    btnPressTime = millis();
    return;
  }

  if (!isDown && pressed) {
    pressed = false;
    unsigned long held = millis() - btnPressTime;

    if (held >= LONG_PRESS_MS) {
      // ── TOQUE LONGO → toggle do canal selecionado ──
      toggleRelay(selectedChannel);
      bool newState = relayState[selectedChannel];

      if (newState) beepRelayOn(); else beepRelayOff();

      for (int i = 0; i < 2; i++) {
        ledOrange(); delay(80);
        ledOff();    delay(60);
      }
      applyChannelColor();

      if (mqttConnected) publishState(selectedChannel);

    } else {
      // ── TOQUE RÁPIDO → próximo canal ──
      selectedChannel = (selectedChannel + 1) % 8;
      beepChannelChange();

      ledOrange(); delay(100); ledOff(); delay(50);
      applyChannelColor();
    }
  }
}
