#include <WiFi.h>
#include <WiFiManager.h>
#include <FirebaseESP32.h>
#include <ArduinoOTA.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <esp_task_wdt.h>
#include <time.h>

#define FIREBASE_DISABLE_SD_CARD
#define FIREBASE_DISABLE_FLASH
#define FIREBASE_DISABLE_FILE_SYSTEM

// ============================================================
// ESP32-C3 - MONITOR E CONTROLE DA CAIXA D'AGUA - V2.2
// O ESP32 fica responsavel pela automacao e seguranca.
// O GitHub fica responsavel pelo painel visual.
// ============================================================

#define FIREBASE_HOST "monitor-caixa-agua-ff63a-default-rtdb.firebaseio.com"
// IMPORTANTE: use uma credencial valida para legacy_token.
// NAO use a API Key do Firebase (AIza...) neste campo.
#define FIREBASE_AUTH "COLOQUE_SEU_DATABASE_SECRET_AQUI"

// Gere novos tokens antes de colocar em producao.
const char* TELEGRAM_TOKEN = "COLOQUE_NOVO_TOKEN_TELEGRAM";
const char* TELEGRAM_CHAT  = "COLOQUE_SEU_CHAT_ID";
const char* VOICEMONKEY_TOKEN = "COLOQUE_NOVO_TOKEN_VOICEMONKEY";

const char* SONOFF_IP = "192.168.1.71";
const char* SONOFF_USER = "";
const char* SONOFF_PASS = "";

// GPIOs usados atualmente na sua instalacao.
const uint8_t PIN_BOIA_20 = 4;
const uint8_t PIN_BOIA_40 = 5;
const uint8_t PIN_BOIA_60 = 6;
const uint8_t PIN_BOIA_80 = 7;
const uint8_t PIN_BOIA_95 = 10;

const char* FW_VERSION = "2.2";

// Geometria usada no seu painel.
const float R_BASE = 58.0;
const float R_TOPO = 75.5;
const float H_UTIL = 75.0;

// Configuracoes editaveis pelo painel/Firebase.
int nivelLigar = 40;
int nivelDesligar = 95;
uint16_t timeoutBombaMin = 90;
uint16_t debounceSeg = 5;

FirebaseData fbdo;
FirebaseConfig fbConfig;
FirebaseAuth fbAuth;
Preferences prefs;

bool modoManual = false;
bool bombaLigada = false;
bool sonoffOnline = false;
bool erroBoias = false;
bool erroSonoff = false;
bool sistemaSeguro = true;

int nivelAtual = 0;
int nivelBruto = -1;
unsigned long inicioDebounce = 0;
unsigned long inicioBomba = 0;
unsigned long ultimoSonoff = 0;
unsigned long ultimoFirebase = 0;
unsigned long ultimoHeartbeat = 0;
unsigned long ultimaConfig = 0;
unsigned long ultimoComando = 0;
unsigned long ultimaNotificacao = 0;

const unsigned long INTERVALO_SONOFF = 10000UL;
const unsigned long INTERVALO_FIREBASE = 10000UL;
const unsigned long INTERVALO_HEARTBEAT = 30000UL;
const unsigned long INTERVALO_CONFIG = 5000UL;
const unsigned long COOLDOWN_ALERTA = 600000UL;

String ultimoEvento = "Inicializando";

// ------------------------------------------------------------
// WDT compativel com Arduino-ESP32 2.x e 3.x
// ------------------------------------------------------------
void iniciarWatchdog() {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  esp_task_wdt_config_t cfg = {};
  cfg.timeout_ms = 60000;
  cfg.idle_core_mask = 0;
  cfg.trigger_panic = true;
  esp_task_wdt_init(&cfg);
#else
  esp_task_wdt_init(60, true);
#endif
  esp_task_wdt_add(NULL);
}

void alimentarWatchdog() {
  esp_task_wdt_reset();
}

// ------------------------------------------------------------
// Volume aproximado em litros pela geometria da caixa.
// ------------------------------------------------------------
float calcularLitros(int nivel) {
  if (nivel <= 0) return 0.0;
  if (nivel > 100) nivel = 100;

  float h = (nivel / 100.0f) * H_UTIL;
  float raio = R_BASE + (R_TOPO - R_BASE) * (h / H_UTIL);

  float volumeCm3 = (PI * h / 3.0f) *
                    (raio * raio + raio * R_BASE + R_BASE * R_BASE);

  return volumeCm3 / 1000.0f;
}

// ------------------------------------------------------------
// Leitura e validacao da escada de boias.
// Esperado: 00000, 10000, 11000, 11100, 11110, 11111.
// ------------------------------------------------------------
int lerNivelBruto(bool &valida, bool &b20, bool &b40, bool &b60, bool &b80, bool &b95) {
  b20 = digitalRead(PIN_BOIA_20) == LOW;
  b40 = digitalRead(PIN_BOIA_40) == LOW;
  b60 = digitalRead(PIN_BOIA_60) == LOW;
  b80 = digitalRead(PIN_BOIA_80) == LOW;
  b95 = digitalRead(PIN_BOIA_95) == LOW;

  valida = true;
  if (b40 && !b20) valida = false;
  if (b60 && !b40) valida = false;
  if (b80 && !b60) valida = false;
  if (b95 && !b80) valida = false;

  if (!valida) return -1;
  if (b95) return 95;
  if (b80) return 80;
  if (b60) return 60;
  if (b40) return 40;
  if (b20) return 20;
  return 0;
}

void processarBoias() {
  bool valida, b20, b40, b60, b80, b95;
  int novoNivel = lerNivelBruto(valida, b20, b40, b60, b80, b95);

  if (!valida) {
    if (!erroBoias) {
      erroBoias = true;
      sistemaSeguro = false;
      registrarEvento("ERRO: combinacao invalida das boias");
      enviarTelegramComCooldown("🚨 ERRO NAS BOIAS!\nCombinacao impossivel detectada.\nBomba bloqueada por seguranca.");
      avisarAlexa("caixamuitocritica");
    }
    return;
  }

  if (erroBoias) {
    erroBoias = false;
    sistemaSeguro = !erroSonoff;
    registrarEvento("Boias normalizadas");
  }

  if (novoNivel != nivelBruto) {
    nivelBruto = novoNivel;
    inicioDebounce = millis();
    return;
  }

  if (millis() - inicioDebounce < (unsigned long)debounceSeg * 1000UL) return;

  if (nivelAtual != novoNivel) {
    int anterior = nivelAtual;
    nivelAtual = novoNivel;
    registrarEvento("Nivel: " + String(anterior) + "% -> " + String(nivelAtual) + "%");
  }
}

// ------------------------------------------------------------
// Sonoff Tasmota
// ------------------------------------------------------------
bool consultarSonoff() {
  if (WiFi.status() != WL_CONNECTED) {
    sonoffOnline = false;
    erroSonoff = true;
    sistemaSeguro = false;
    return false;
  }

  WiFiClient client;
  HTTPClient http;
  String url = String("http://") + SONOFF_IP + "/cm?cmnd=Power";

  http.setTimeout(3000);
  if (!http.begin(client, url)) {
    sonoffOnline = false;
    erroSonoff = true;
    sistemaSeguro = false;
    return false;
  }

  if (strlen(SONOFF_USER) > 0) http.setAuthorization(SONOFF_USER, SONOFF_PASS);

  int code = http.GET();
  String resposta = (code == HTTP_CODE_OK) ? http.getString() : "";
  http.end();

  if (code != HTTP_CODE_OK) {
    sonoffOnline = false;
    erroSonoff = true;
    sistemaSeguro = false;
    return false;
  }

  bool achouEstado = false;
  if (resposta.indexOf("\"ON\"") >= 0) {
    bombaLigada = true;
    achouEstado = true;
  } else if (resposta.indexOf("\"OFF\"") >= 0) {
    bombaLigada = false;
    achouEstado = true;
  }

  sonoffOnline = achouEstado;
  erroSonoff = !achouEstado;
  sistemaSeguro = !erroBoias && !erroSonoff;
  return achouEstado;
}

bool comandarSonoff(bool ligar) {
  if (WiFi.status() != WL_CONNECTED) return false;

  String comando = ligar ? "Power%20On" : "Power%20Off";
  String url = String("http://") + SONOFF_IP + "/cm?cmnd=" + comando;

  for (uint8_t tentativa = 0; tentativa < 2; tentativa++) {
    WiFiClient client;
    HTTPClient http;
    http.setTimeout(3000);

    if (!http.begin(client, url)) continue;
    if (strlen(SONOFF_USER) > 0) http.setAuthorization(SONOFF_USER, SONOFF_PASS);

    int code = http.GET();
    http.end();

    if (code == HTTP_CODE_OK) {
      delay(250);
      if (consultarSonoff() && bombaLigada == ligar) {
        erroSonoff = false;
        sistemaSeguro = !erroBoias;
        return true;
      }
    }

    alimentarWatchdog();
  }

  sonoffOnline = false;
  erroSonoff = true;
  sistemaSeguro = false;
  return false;
}

// ------------------------------------------------------------
// Controle automatico e timeout.
// ------------------------------------------------------------
void controlarAutomatico() {
  if (modoManual || erroBoias) return;

  if (nivelAtual <= nivelLigar && !bombaLigada) {
    if (!sonoffOnline) {
      if (!consultarSonoff()) {
        registrarEvento("Bomba solicitada, Sonoff offline");
        return;
      }
    }

    if (comandarSonoff(true)) {
      bombaLigada = true;
      inicioBomba = millis();
      registrarEvento("Bomba LIGADA automaticamente em " + String(nivelAtual) + "%");
      enviarTelegramComCooldown("⚠️ Bomba LIGADA automaticamente.\nNivel: " + String(nivelAtual) + "%");
      avisarAlexa("ligarbomba");
    } else {
      registrarEvento("FALHA: nao foi possivel ligar a bomba");
    }
  }

  if (nivelAtual >= nivelDesligar && bombaLigada) {
    if (comandarSonoff(false)) {
      bombaLigada = false;
      registrarEvento("Bomba DESLIGADA: nivel atingiu " + String(nivelAtual) + "%");
      enviarTelegramComCooldown("🔔 Caixa d'agua cheia: " + String(nivelAtual) + "%.\nBomba desligada.");
      avisarAlexa("caixacheia");
    } else {
      registrarEvento("ERRO: nao foi possivel confirmar desligamento");
    }
  }
}

void verificarTimeout() {
  if (!bombaLigada) return;

  unsigned long limite = (unsigned long)timeoutBombaMin * 60000UL;
  if (millis() - inicioBomba < limite) return;

  registrarEvento("TIMEOUT: bomba desligada apos " + String(timeoutBombaMin) + " minutos");

  bool desligou = comandarSonoff(false);
  bombaLigada = false;

  enviarTelegramComCooldown("🚨 SEGURANCA!\nA bomba ultrapassou " + String(timeoutBombaMin) + " minutos.\nOrdem de desligamento enviada.");
  avisarAlexa("caixamuitocritica");

  if (!desligou) {
    erroSonoff = true;
    sistemaSeguro = false;
    enviarTelegramComCooldown("🚨 ATENCAO! Nao foi possivel confirmar o desligamento do Sonoff.");
  }
}

// ------------------------------------------------------------
// Firebase - configuracoes editaveis no painel.
// ------------------------------------------------------------
void lerConfiguracoes() {
  if (!Firebase.ready()) return;

  int v;

  if (Firebase.getInt(fbdo, "/configuracao/nivel_ligar")) {
    v = fbdo.intData();
    if (v >= 20 && v <= 80 && v < nivelDesligar) {
      nivelLigar = v;
      prefs.putInt("nivelLigar", nivelLigar);
    }
  }

  if (Firebase.getInt(fbdo, "/configuracao/nivel_desligar")) {
    v = fbdo.intData();
    if (v >= 80 && v <= 95 && v > nivelLigar) {
      nivelDesligar = v;
      prefs.putInt("nivelDesligar", nivelDesligar);
    }
  }

  if (Firebase.getInt(fbdo, "/configuracao/timeout_bomba")) {
    v = fbdo.intData();
    if (v >= 5 && v <= 180) {
      timeoutBombaMin = (uint16_t)v;
      prefs.putUShort("timeout", timeoutBombaMin);
    }
  }

  if (Firebase.getInt(fbdo, "/configuracao/debounce")) {
    v = fbdo.intData();
    if (v >= 1 && v <= 30) {
      debounceSeg = (uint16_t)v;
      prefs.putUShort("debounce", debounceSeg);
    }
  }
}

void processarComandos() {
  if (!Firebase.ready()) return;
  if (millis() - ultimoComando < 1000) return;

  if (Firebase.getString(fbdo, "/comandos/bomba")) {
    String cmd = fbdo.stringData();
    cmd.toUpperCase();

    if (cmd == "ON" || cmd == "OFF") {
      ultimoComando = millis();

      if (cmd == "ON") {
        modoManual = true;
        if (comandarSonoff(true)) {
          bombaLigada = true;
          inicioBomba = millis();
          registrarEvento("Bomba LIGADA pelo painel (manual)");
        }
      } else {
        if (comandarSonoff(false)) {
          bombaLigada = false;
          registrarEvento("Bomba DESLIGADA pelo painel");
        }
      }

      Firebase.deleteNode(fbdo, "/comandos/bomba");
    }
  }

  if (Firebase.getString(fbdo, "/comandos/modo")) {
    String cmd = fbdo.stringData();
    cmd.toUpperCase();

    if (cmd == "MANUAL" || cmd == "AUTO") {
      ultimoComando = millis();
      modoManual = (cmd == "MANUAL");
      registrarEvento(modoManual ? "Modo MANUAL" : "Modo AUTOMATICO");
      Firebase.deleteNode(fbdo, "/comandos/modo");
    }
  }
}

// ------------------------------------------------------------
// Firebase - estado.
// ------------------------------------------------------------
void publicarEstado() {
  if (!Firebase.ready()) return;

  Firebase.setInt(fbdo, "/nivel", nivelAtual);
  Firebase.setFloat(fbdo, "/litros", calcularLitros(nivelAtual));
  Firebase.setBool(fbdo, "/status_bomba", bombaLigada);
  Firebase.setBool(fbdo, "/modo_manual", modoManual);
  Firebase.setBool(fbdo, "/sonoff_online", sonoffOnline);
  Firebase.setBool(fbdo, "/erro_boias", erroBoias);
  Firebase.setBool(fbdo, "/erro_sonoff", erroSonoff);
  Firebase.setBool(fbdo, "/sistema_seguro", sistemaSeguro);
  Firebase.setString(fbdo, "/ultimo_evento", ultimoEvento);

  Firebase.setBool(fbdo, "/boias/20", digitalRead(PIN_BOIA_20) == LOW);
  Firebase.setBool(fbdo, "/boias/40", digitalRead(PIN_BOIA_40) == LOW);
  Firebase.setBool(fbdo, "/boias/60", digitalRead(PIN_BOIA_60) == LOW);
  Firebase.setBool(fbdo, "/boias/80", digitalRead(PIN_BOIA_80) == LOW);
  Firebase.setBool(fbdo, "/boias/95", digitalRead(PIN_BOIA_95) == LOW);

  Firebase.setInt(fbdo, "/configuracao/nivel_ligar", nivelLigar);
  Firebase.setInt(fbdo, "/configuracao/nivel_desligar", nivelDesligar);
  Firebase.setInt(fbdo, "/configuracao/timeout_bomba", timeoutBombaMin);
  Firebase.setInt(fbdo, "/configuracao/debounce", debounceSeg);
}

void publicarHeartbeat() {
  if (!Firebase.ready()) return;

  Firebase.setString(fbdo, "/dispositivo/status", "online");
  Firebase.setInt(fbdo, "/dispositivo/rssi", WiFi.RSSI());
  Firebase.setString(fbdo, "/dispositivo/ip", WiFi.localIP().toString());
  Firebase.setULong(fbdo, "/dispositivo/uptime", millis() / 1000UL);
  Firebase.setString(fbdo, "/dispositivo/versao", FW_VERSION);
  Firebase.setULong(fbdo, "/ultimo_ping", millis());
}

void registrarEvento(const String &evento) {
  ultimoEvento = evento;
  Serial.println("EVENTO: " + evento);

  if (!Firebase.ready()) return;

  FirebaseJson json;
  json.set("evento", evento);
  json.set("nivel", nivelAtual);
  json.set("litros", calcularLitros(nivelAtual));
  json.set("bomba", bombaLigada);
  json.set("modo", modoManual ? "MANUAL" : "AUTOMATICO");
  json.set("uptime", millis() / 1000UL);
  json.set("epoch", (unsigned long)time(nullptr));
  Firebase.pushJSON(fbdo, "/eventos", json);
}

// ------------------------------------------------------------
// Notificacoes. O navegador NAO possui mais tokens.
// ------------------------------------------------------------
void enviarTelegramComCooldown(const String &msg) {
  if (millis() - ultimaNotificacao < COOLDOWN_ALERTA) return;
  ultimaNotificacao = millis();

  if (WiFi.status() != WL_CONNECTED) return;
  if (strlen(TELEGRAM_TOKEN) < 10) return;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;

  String url = String("https://api.telegram.org/bot") + TELEGRAM_TOKEN +
               "/sendMessage?chat_id=" + TELEGRAM_CHAT +
               "&text=" + urlEncode(msg);

  http.setTimeout(5000);
  if (http.begin(client, url)) {
    http.GET();
    http.end();
  }
}

void avisarAlexa(const String &device) {
  if (WiFi.status() != WL_CONNECTED) return;
  if (strlen(VOICEMONKEY_TOKEN) < 10) return;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;

  String url = String("https://api-v2.voicemonkey.io/trigger?token=") +
               VOICEMONKEY_TOKEN + "&device=" + device + "&monkey=" + device;

  http.setTimeout(5000);
  if (http.begin(client, url)) {
    http.GET();
    http.end();
  }
}

String urlEncode(const String &str) {
  String encoded;
  const char *hex = "0123456789ABCDEF";

  for (size_t i = 0; i < str.length(); i++) {
    uint8_t c = (uint8_t)str[i];
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      encoded += (char)c;
    } else {
      encoded += '%';
      encoded += hex[(c >> 4) & 0x0F];
      encoded += hex[c & 0x0F];
    }
  }
  return encoded;
}

// ------------------------------------------------------------
// Setup
// ------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(300);

  iniciarWatchdog();

  pinMode(PIN_BOIA_20, INPUT_PULLUP);
  pinMode(PIN_BOIA_40, INPUT_PULLUP);
  pinMode(PIN_BOIA_60, INPUT_PULLUP);
  pinMode(PIN_BOIA_80, INPUT_PULLUP);
  pinMode(PIN_BOIA_95, INPUT_PULLUP);

  prefs.begin("caixa", false);
  nivelLigar = prefs.getInt("nivelLigar", 40);
  nivelDesligar = prefs.getInt("nivelDesligar", 95);
  timeoutBombaMin = prefs.getUShort("timeout", 90);
  debounceSeg = prefs.getUShort("debounce", 5);

  if (nivelLigar >= nivelDesligar) {
    nivelLigar = 40;
    nivelDesligar = 95;
  }

  WiFi.mode(WIFI_STA);
  WiFiManager wm;

  // Se ja houver credenciais salvas, conecta sem abrir portal.
  // Se nao houver, cria o portal Caixa_Agua_Boias.
  if (!wm.autoConnect("Caixa_Agua_Boias", "Config123")) {
    delay(2000);
    ESP.restart();
  }

  Serial.println("\n=== MONITOR CAIXA D'AGUA V2.2 ===");
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());

  ArduinoOTA.setHostname("Monitor-Boias-Nilopolis");
  ArduinoOTA.setPassword("ALTERE_SUA_SENHA_OTA");
  ArduinoOTA.begin();

  configTime(-3 * 3600, 0, "pool.ntp.org", "a.st1.ntp.br", "time.nist.gov");

  fbConfig.host = FIREBASE_HOST;
  fbConfig.signer.tokens.legacy_token = FIREBASE_AUTH;
  Firebase.begin(&fbConfig, &fbAuth);
  Firebase.reconnectWiFi(true);

  // Primeiro sincroniza com o Sonoff.
  consultarSonoff();
  publicarEstado();
  publicarHeartbeat();
  registrarEvento("ESP32-C3 V2.2 iniciado");

  // Nao assume que a bomba esta desligada apos reboot.
  // Se o Sonoff estiver ON, o timeout passa a contar a partir do reboot.
  if (bombaLigada) inicioBomba = millis();
}

// ------------------------------------------------------------
// Loop
// ------------------------------------------------------------
void loop() {
  alimentarWatchdog();
  ArduinoOTA.handle();

  unsigned long agora = millis();

  processarBoias();

  if (agora - ultimaConfig >= INTERVALO_CONFIG) {
    ultimaConfig = agora;
    lerConfiguracoes();
  }

  processarComandos();

  if (agora - ultimoSonoff >= INTERVALO_SONOFF) {
    ultimoSonoff = agora;
    consultarSonoff();
  }

  controlarAutomatico();
  verificarTimeout();

  if (agora - ultimoFirebase >= INTERVALO_FIREBASE) {
    ultimoFirebase = agora;
    publicarEstado();
  }

  if (agora - ultimoHeartbeat >= INTERVALO_HEARTBEAT) {
    ultimoHeartbeat = agora;
    publicarHeartbeat();
  }

  delay(20);
}
