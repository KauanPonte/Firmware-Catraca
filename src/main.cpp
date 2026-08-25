#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <SPI.h>
#include <MFRC522.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// =====================================================
// CATRACA COM RFID + MATRÍCULA + WIFI + LCD
// =====================================================

// ==========================
// LCD
// ==========================
LiquidCrystal_I2C lcd(0x27, 16, 2);

// ==========================
// TECLADO
// ==========================
const uint8_t rowPins[4] = {33, 32, 35, 34};
const uint8_t colPins[4] = {14, 27, 26, 25};

char keys[4][4] = {
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'*','0','#','D'}
};

// ==========================
// PINOS CATRACA
// ==========================
const int sensorEsq = 2;
const int sensorDir  = 4;
const int pinRelay   = 15;
const int BUZZER_PIN = 5;

// ==========================
// RFID
// ==========================
const int SS_PIN  = 21;
const int RST_PIN = 22;
MFRC522 mfrc522(SS_PIN, RST_PIN);

// ==========================
// WIFI / SERVIDOR
// ==========================
const char* ssid     = "LAR-IFCE";
const char* password = "if.LAR@2024";
String serverUrl     = "https://ifaccess.iracema.xyz/acesso";
String matriculaUrl  = "https://ifaccess.iracema.xyz/matricula";

// ==========================
// SERVIDOR WEB DE DIAGNOSTICO (TIMING)
// ==========================
WebServer diagServer(80);

struct TimingRecord {
  String origem;
  int rssi;
  long dnsMs;
  long tcpTesteMs;
  long conectarMs;
  long enviarMs;
  long respostaMs;
  long downloadMs;
  long fecharMs;
  long totalMs;
  bool ok;
};

const int MAX_TIMING_RECORDS = 15;
TimingRecord timingRecords[MAX_TIMING_RECORDS];
int timingCount = 0;
int timingNext  = 0;

void registrarTiming(const TimingRecord &r) {
  timingRecords[timingNext] = r;
  timingNext = (timingNext + 1) % MAX_TIMING_RECORDS;
  if (timingCount < MAX_TIMING_RECORDS) timingCount++;
}

void handleTimingPage() {
  String html = "<html><head><meta http-equiv='refresh' content='3'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<title>Diagnostico Catraca</title>";
  html += "<style>body{font-family:sans-serif}.wrap{overflow-x:auto}table{border-collapse:collapse;min-width:900px}";
  html += "td,th{border:1px solid #ccc;padding:6px;text-align:right;white-space:nowrap}th:first-child,td:first-child{text-align:left}";
  html += "td:last-child{font-weight:bold}</style></head><body>";
  html += "<h3>Diagnostico de tempo - Catraca</h3>";
  html += "<p>IP: " + WiFi.localIP().toString() + " | Uptime: " + String(millis() / 1000) + "s";
  html += " | WiFi: " + String(WiFi.status() == WL_CONNECTED ? "conectado" : "SEM CONEXAO") + "</p>";
  html += "<p style='font-size:12px;color:#666'>DNS: resolver nome->IP | TCP teste: conexao TCP crua descartavel (mede so a rede) | "
          "Conectar: TCP+TLS reais usados na requisicao | Enviar: escrever o POST no socket | "
          "Resposta servidor: tempo ate o 1o byte chegar (TTFB, indica o servidor pensando) | Download: baixar o corpo | Fechar: encerrar conexao</p>";
  html += "<div class='wrap'><table><tr><th>Origem</th><th>RSSI</th><th>DNS</th><th>TCP teste</th><th>Conectar(TCP+TLS)</th>"
          "<th>Enviar</th><th>Resposta servidor</th><th>Download</th><th>Fechar</th><th>TOTAL</th></tr>";

  for (int i = 0; i < timingCount; i++) {
    int idx = (timingNext - 1 - i + MAX_TIMING_RECORDS) % MAX_TIMING_RECORDS;
    TimingRecord &r = timingRecords[idx];
    html += "<tr><td>" + r.origem + (r.ok ? "" : " (falhou)") + "</td><td>" + String(r.rssi) + "dBm</td>";
    html += "<td>" + String(r.dnsMs) + "ms</td><td>" + String(r.tcpTesteMs) + "ms</td><td>" + String(r.conectarMs) + "ms</td>";
    html += "<td>" + String(r.enviarMs) + "ms</td><td>" + String(r.respostaMs) + "ms</td><td>" + String(r.downloadMs) + "ms</td>";
    html += "<td>" + String(r.fecharMs) + "ms</td><td>" + String(r.totalMs) + "ms</td></tr>";
  }

  if (timingCount == 0) html += "<tr><td colspan='10'>Nenhuma leitura ainda</td></tr>";
  html += "</table></div></body></html>";

  diagServer.send(200, "text/html", html);
}

// ==========================
// REQUISICAO HTTPS MANUAL COM TIMING DETALHADO
// ==========================
void parseUrl(String url, String &host, String &path) {
  int hostStart = url.indexOf("://");
  hostStart = (hostStart < 0) ? 0 : hostStart + 3;
  int pathStart = url.indexOf('/', hostStart);
  if (pathStart < 0) {
    host = url.substring(hostStart);
    path = "/";
  } else {
    host = url.substring(hostStart, pathStart);
    path = url.substring(pathStart);
  }
}

// ==========================
// CLIENTE HTTPS REUTILIZADO
// (mantido conectado entre requisicoes para evitar
//  refazer o handshake TLS a cada leitura de cartao)
// ==========================
WiFiClientSecure secureClient;
String secureClientHost = "";
bool secureClientPronto = false;
const unsigned long HTTP_TIMEOUT_MS = 3000;

int extrairContentLength(const String &headers) {
  int idx = headers.indexOf("Content-Length:");
  if (idx < 0) idx = headers.indexOf("content-length:");
  if (idx < 0) return -1;
  int lineEnd = headers.indexOf("\r\n", idx);
  String valor = headers.substring(idx + String("Content-Length:").length(), lineEnd);
  valor.trim();
  return valor.toInt();
}

bool respostaFechaConexao(String headers) {
  headers.toLowerCase();
  return headers.indexOf("connection: close") >= 0;
}

int fazerRequisicaoComTiming(String url, String jsonBody, String origem, String &respostaOut) {
  String host, path;
  parseUrl(url, host, path);

  TimingRecord r;
  r.origem = origem;
  r.rssi   = WiFi.RSSI();
  r.ok     = false;

  unsigned long t0 = millis();

  IPAddress ip;
  unsigned long tA = millis();
  bool dnsOk = WiFi.hostByName(host.c_str(), ip);
  r.dnsMs = millis() - tA;

  unsigned long tC = millis();
  {
    WiFiClient rawClient;
    if (dnsOk) rawClient.connect(ip, 443);
    else       rawClient.connect(host.c_str(), 443);
    rawClient.stop();
  }
  r.tcpTesteMs = millis() - tC;

  if (!secureClientPronto) {
    secureClient.setInsecure();
    secureClient.setTimeout(HTTP_TIMEOUT_MS / 1000);
    secureClient.setHandshakeTimeout(HTTP_TIMEOUT_MS);
    secureClientPronto = true;
  }

  unsigned long tE = millis();
  bool reaproveitada = secureClient.connected() && secureClientHost == host;
  bool connected = reaproveitada;
  if (!connected) {
    secureClient.stop();
    connected = dnsOk ? secureClient.connect(ip, 443, HTTP_TIMEOUT_MS) : secureClient.connect(host.c_str(), 443, HTTP_TIMEOUT_MS);
    if (connected) secureClientHost = host;
  }
  r.conectarMs = millis() - tE;

  if (!connected) {
    r.enviarMs = r.respostaMs = r.downloadMs = r.fecharMs = 0;
    r.totalMs  = millis() - t0;
    registrarTiming(r);
    respostaOut = "";
    secureClientHost = "";
    return -1;
  }

  String request = "POST " + path + " HTTP/1.1\r\n";
  request += "Host: " + host + "\r\n";
  request += "Content-Type: application/json\r\n";
  request += "Content-Length: " + String(jsonBody.length()) + "\r\n";
  request += "Connection: keep-alive\r\n\r\n";
  request += jsonBody;

  unsigned long tG = millis();
  size_t escritos = secureClient.print(request);
  r.enviarMs = millis() - tG;

  if (escritos == 0 && reaproveitada) {
    // conexao reaproveitada estava morta do lado do servidor — reconecta e tenta 1x
    secureClient.stop();
    connected = dnsOk ? secureClient.connect(ip, 443) : secureClient.connect(host.c_str(), 443);
    if (connected) {
      secureClientHost = host;
      tG = millis();
      escritos = secureClient.print(request);
      r.enviarMs = millis() - tG;
    }
  }

  if (!connected || escritos == 0) {
    r.respostaMs = r.downloadMs = r.fecharMs = 0;
    r.totalMs = millis() - t0;
    registrarTiming(r);
    respostaOut = "";
    secureClient.stop();
    secureClientHost = "";
    return -1;
  }

  unsigned long tI = millis();
  unsigned long limite = tI + HTTP_TIMEOUT_MS;
  while (!secureClient.available() && secureClient.connected() && millis() < limite) {
    delay(1);
  }
  r.respostaMs = millis() - tI;

  unsigned long tK = millis();

  String headers = "";
  bool headersCompletos = false;
  unsigned long limiteHeaders = millis() + HTTP_TIMEOUT_MS;
  while (!headersCompletos && millis() < limiteHeaders) {
    while (secureClient.available()) {
      headers += (char)secureClient.read();
      if (headers.endsWith("\r\n\r\n")) { headersCompletos = true; break; }
    }
    if (!headersCompletos && !secureClient.connected() && !secureClient.available()) break;
  }

  int contentLength = extrairContentLength(headers);
  bool chunked = headers.indexOf("Transfer-Encoding: chunked") >= 0 ||
                 headers.indexOf("transfer-encoding: chunked") >= 0;
  bool fechaConexao = respostaFechaConexao(headers) || chunked || contentLength < 0;

  String corpo = "";
  if (!fechaConexao) {
    unsigned long limiteBody = millis() + HTTP_TIMEOUT_MS;
    while ((int)corpo.length() < contentLength && millis() < limiteBody) {
      while (secureClient.available() && (int)corpo.length() < contentLength) {
        corpo += (char)secureClient.read();
      }
      if (!secureClient.connected() && !secureClient.available() && (int)corpo.length() < contentLength) break;
    }
  } else {
    unsigned long limiteFechamento = millis() + HTTP_TIMEOUT_MS;
    while ((secureClient.connected() || secureClient.available()) && millis() < limiteFechamento) {
      while (secureClient.available()) corpo += (char)secureClient.read();
      if (!secureClient.connected() && !secureClient.available()) break;
    }
    if (millis() >= limiteFechamento) {
      secureClient.stop();
      secureClientHost = "";
    }
  }
  r.downloadMs = millis() - tK;

  unsigned long tM = millis();
  if (fechaConexao) {
    secureClient.stop();
    secureClientHost = "";
  }
  r.fecharMs = millis() - tM;

  r.totalMs = millis() - t0;
  r.ok = true;
  registrarTiming(r);

  int code = 0;
  int sp1 = headers.indexOf(' ');
  if (headers.startsWith("HTTP/") && sp1 > 0) {
    int sp2 = headers.indexOf(' ', sp1 + 1);
    code = headers.substring(sp1 + 1, sp2).toInt();
  }

  respostaOut = corpo;
  return code;
}

// ==========================
// ESTADO DA CATRACA
// ==========================
enum EstadoRotacao { IDLE, HORARIO, ANTIHORARIO };
EstadoRotacao estado = IDLE;

unsigned long lastCardRead  = 0;
const unsigned long releaseTime  = 5000;
const unsigned long cardCooldown = 3000;
bool creditoRotacao = false;
String lastUID = "";

// ==========================
// ESTADO DO TECLADO
// ==========================
String matriculaBuffer = "";
bool aguardandoMatricula = false;

// ==========================
// TIMER LCD
// ==========================
unsigned long lcdTimer = 0;
bool lcdPendente = false;

// ==========================
// BUZZER NÃO-BLOQUEANTE
// ==========================
#define BUZZER_ATIVO true

struct BeepState {
  int vezes;
  int durOn;
  int durOff;
  int step;
  unsigned long nextMs;
  bool active;
};
BeepState beep = {0, 0, 0, 0, 0, false};

void iniciarBeep(int vezes, int durOn, int durOff) {
  digitalWrite(BUZZER_PIN, LOW);
  beep = {vezes, durOn, durOff, 0, millis(), true};
}

void processarBeep() {
  if (!beep.active) return;
  if (millis() < beep.nextMs) return;

  bool faseOn = (beep.step % 2 == 0);

  if (faseOn) {
    if (BUZZER_ATIVO) digitalWrite(BUZZER_PIN, HIGH);
    else              tone(BUZZER_PIN, 2800, beep.durOn);
    beep.nextMs = millis() + beep.durOn;
  } else {
    if (BUZZER_ATIVO) digitalWrite(BUZZER_PIN, LOW);
    else              noTone(BUZZER_PIN);
    int beepsDone = (beep.step + 1) / 2;
    if (beepsDone >= beep.vezes) { beep.active = false; return; }
    beep.nextMs = millis() + beep.durOff;
  }
  beep.step++;
}

// ==========================
// PROTÓTIPOS
// ==========================
char    readKey();
void    enviarUID(String uid);
void    enviarMatricula(String matricula);
void    processarResposta(int httpCode, String response);
void    mostrarLCD(String linha1, String linha2, bool limpar = true);
void    lcdIdle();
void    agendarLcdIdle();
void    somLiberado();
void    somNegado();
void    somLeitura();
String  extrairCampo(String json, String campo);

// =====================================================
// SETUP
// =====================================================
void setup() {
  Serial.begin(115200);

  pinMode(sensorEsq, INPUT_PULLUP);
  pinMode(sensorDir,  INPUT_PULLUP);
  pinMode(pinRelay,   OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(pinRelay,   LOW);
  digitalWrite(BUZZER_PIN, LOW);
  noTone(BUZZER_PIN);

  pinMode(34, INPUT);
  pinMode(35, INPUT);
  pinMode(32, INPUT_PULLUP);
  pinMode(33, INPUT_PULLUP);

  for (int i = 0; i < 4; i++) {
    pinMode(colPins[i], OUTPUT);
    digitalWrite(colPins[i], HIGH);
  }

  Wire.begin(12, 13);
  lcd.init();
  lcd.backlight();

  SPI.begin(18, 19, 23, 21);
  mfrc522.PCD_Init();

  mostrarLCD("Conectando", "WiFi...");
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);
  WiFi.begin(ssid, password);

  int tentativas = 0;
  while (WiFi.status() != WL_CONNECTED && tentativas < 20) {
    delay(500);
    Serial.print(".");
    tentativas++;
  }

  if (WiFi.status() != WL_CONNECTED) {
    mostrarLCD("Sem WiFi!", "Verifique rede");
    Serial.println("Falha no WiFi!");
  } else {
    Serial.println("\nWiFi conectado!");
    mostrarLCD("WiFi OK!", WiFi.localIP().toString());
    delay(1500);
  }

  if (WiFi.status() == WL_CONNECTED) {
    diagServer.on("/", handleTimingPage);
    diagServer.on("/timing", handleTimingPage);
    diagServer.begin();

    if (MDNS.begin("catraca")) {
      MDNS.addService("http", "tcp", 80);
      Serial.println("Diagnostico web em: http://catraca.local");
    } else {
      Serial.println("Falha ao iniciar mDNS");
    }
    Serial.println("Diagnostico web tambem em: http://" + WiFi.localIP().toString());
  }

  lcdIdle();
  noTone(BUZZER_PIN);
  Serial.println("Sistema iniciado");
}

// =====================================================
// LOOP
// =====================================================
void loop() {

  // ─────────────────────────────────────────────────
  // 0. BUZZER + SERVIDOR DE DIAGNOSTICO
  // ─────────────────────────────────────────────────
  processarBeep();
  diagServer.handleClient();

  // ─────────────────────────────────────────────────
  // 1. WIFI — reconecta se necessário
  // ─────────────────────────────────────────────────
  static unsigned long lastWifiCheck = 0;
  if (millis() - lastWifiCheck > 5000) {
    lastWifiCheck = millis();
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi perdido — reconectando...");
      WiFi.reconnect();
    }
  }

  // ─────────────────────────────────────────────────
  // 2. TIMER LCD
  // ─────────────────────────────────────────────────
  if (lcdPendente && millis() - lcdTimer > 3000) {
    lcdIdle();
  }

  // ─────────────────────────────────────────────────
  // 3. TECLADO
  // ─────────────────────────────────────────────────
  char key = readKey();

  if (key != '\0') {
    Serial.print("Tecla: "); Serial.println(key);

    if (key == '#') {
      if (matriculaBuffer.length() > 0) {
        mostrarLCD("Consultando...", matriculaBuffer);
        enviarMatricula(matriculaBuffer);
        matriculaBuffer = "";
        aguardandoMatricula = false;
      }
    } else if (key == '*') {
      matriculaBuffer = "";
      aguardandoMatricula = false;
      lcdIdle();
    } else if (key >= '0' && key <= '9') {
      if (!aguardandoMatricula) {
        aguardandoMatricula = true;
        mostrarLCD("Matricula:", "");
      }
      matriculaBuffer += key;
      lcd.setCursor(0, 1);
      lcd.print(matriculaBuffer + "          ");
    }
  }

  // ─────────────────────────────────────────────────
  // 4. RFID
  // ─────────────────────────────────────────────────
  if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) {

    String uid = "";
    for (byte i = 0; i < mfrc522.uid.size; i++) {
      if (mfrc522.uid.uidByte[i] < 0x10) uid += "0";
      uid += String(mfrc522.uid.uidByte[i], HEX);
    }
    uid.toUpperCase();

    mfrc522.PICC_HaltA();
    mfrc522.PCD_StopCrypto1();

    if (uid == lastUID && millis() - lastCardRead < cardCooldown) {
      Serial.println("Cartão ignorado (cooldown): " + uid);
    } else {
      lastUID = uid;
      Serial.println("Cartão UID: " + uid);
      mostrarLCD("Lendo cartao...", uid);
      unsigned long tLoopStart = millis();
      enviarUID(uid);
      Serial.printf("[TIMING LOOP] leitura->fim_enviarUID = %lums\n", millis() - tLoopStart);
      matriculaBuffer = "";
      aguardandoMatricula = false;
    }
  }

  // ─────────────────────────────────────────────────
  // 5. CRÉDITO VÁLIDO?
  // ─────────────────────────────────────────────────
  bool liberado = creditoRotacao && (millis() - lastCardRead < releaseTime);
  if (!liberado) creditoRotacao = false;

  // ─────────────────────────────────────────────────
  // 6. SENSORES
  // ─────────────────────────────────────────────────
  bool esqAtivo = digitalRead(sensorEsq) == LOW;
  bool dirAtivo = digitalRead(sensorDir) == LOW;
  bool repouso  = esqAtivo && dirAtivo;

  // ─────────────────────────────────────────────────
  // 7. MÁQUINA DE ESTADOS
  // ─────────────────────────────────────────────────
  switch (estado) {

    case IDLE:
      digitalWrite(pinRelay, LOW);

      if (dirAtivo && !esqAtivo) {
        creditoRotacao = false;
        lastCardRead = 0;
        estado = HORARIO;
        Serial.println("HORARIO iniciado");
      }
      else if (esqAtivo && !dirAtivo) {
        estado = ANTIHORARIO;
        Serial.println("ANTI-HORARIO iniciado");
        if (!liberado) {
          digitalWrite(pinRelay, HIGH);
          Serial.println("RELE ATIVADO");
        }
      }
      break;

    case HORARIO:
      if (repouso) {
        estado = IDLE;
        Serial.println("FIM HORARIO");
        lcdIdle();
      }
      break;

    case ANTIHORARIO:
      if (!liberado) digitalWrite(pinRelay, HIGH);

      if (repouso) {
        if (liberado) {
          creditoRotacao = false;
          Serial.println("Credito consumido");
        }
        digitalWrite(pinRelay, LOW);
        estado = IDLE;
        Serial.println("FIM ANTI-HORARIO");
        lcdIdle();
      }
      break;
  }
}

// =====================================================
// ENVIA UID (RFID)
// =====================================================
void enviarUID(String uid) {
  if (WiFi.status() != WL_CONNECTED) {
    mostrarLCD("Sem WiFi!", "Acesso negado");
    somNegado();
    agendarLcdIdle();
    return;
  }

  String body = "{\"uid\":\"" + uid + "\"}";
  String resp;
  int code = fazerRequisicaoComTiming(serverUrl, body, "RFID:" + uid, resp);

  Serial.println("Resp RFID: " + resp);
  processarResposta(code, resp);
}

// =====================================================
// ENVIA MATRÍCULA (TECLADO)
// =====================================================
void enviarMatricula(String matricula) {
  if (WiFi.status() != WL_CONNECTED) {
    mostrarLCD("Sem WiFi!", "Acesso negado");
    somNegado();
    agendarLcdIdle();
    return;
  }

  String body = "{\"matricula\":\"" + matricula + "\"}";
  String resp;
  int code = fazerRequisicaoComTiming(matriculaUrl, body, "MAT:" + matricula, resp);

  Serial.println("Resp Matricula: " + resp);
  processarResposta(code, resp);
}

// =====================================================
// PROCESSA RESPOSTA
// =====================================================
void processarResposta(int httpCode, String response) {
  if (httpCode <= 0) {
    mostrarLCD("Erro servidor", "Acesso negado");
    somNegado();
    agendarLcdIdle();
    return;
  }

  if (response.indexOf("\"resultado\":\"leitura\"") >= 0) {
    mostrarLCD("Modo cadastro", "UID capturado");
    somLeitura();
    agendarLcdIdle();
    return;
  }

  String nome      = extrairCampo(response, "nome");
  String matricula = extrairCampo(response, "matricula");

  if (nome.length() > 16)      nome      = nome.substring(0, 16);
  if (matricula.length() > 16) matricula = matricula.substring(0, 16);

  if (response.indexOf("\"liberar\":true") >= 0) {
    String l1 = nome.length() > 0 ? nome : "Liberado!";
    String l2 = matricula.length() > 0 ? matricula : "";
    mostrarLCD(l1, l2);

    somLiberado();
    creditoRotacao = true;
    lastCardRead   = millis();
    agendarLcdIdle();

  } else {
    String l1 = "BLOQUEADO";
    String l2 = nome.length() > 0 ? nome : (matricula.length() > 0 ? matricula : "Nao cadastrado");
    mostrarLCD(l1, l2);

    somNegado();
    agendarLcdIdle();
  }
}

// =====================================================
// LCD
// =====================================================
void lcdIdle() {
  mostrarLCD("Aproxime cartao", "ou matricula:#");
  lcdPendente = false;
}

void agendarLcdIdle() {
  lcdTimer    = millis();
  lcdPendente = true;
}

void mostrarLCD(String linha1, String linha2, bool limpar) {
  if (limpar) lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(linha1);
  lcd.setCursor(0, 1);
  lcd.print(linha2);
}

// =====================================================
// EXTRAI CAMPO JSON
// =====================================================
String extrairCampo(String json, String campo) {
  String chave = "\"" + campo + "\":\"";
  int ini = json.indexOf(chave);
  if (ini < 0) return "";
  ini += chave.length();
  int fim = json.indexOf("\"", ini);
  if (fim < 0) return "";
  return json.substring(ini, fim);
}

// =====================================================
// TECLADO
// =====================================================
char readKey() {
  for (int col = 0; col < 4; col++) {
    for (int i = 0; i < 4; i++) digitalWrite(colPins[i], HIGH);
    digitalWrite(colPins[col], LOW);
    delayMicroseconds(5);
    for (int row = 0; row < 4; row++) {
      if (digitalRead(rowPins[row]) == LOW) {
        delay(50);
        while (digitalRead(rowPins[row]) == LOW);
        return keys[row][col];
      }
    }
  }
  return '\0';
}

// =====================================================
// SONS
// =====================================================
void somLiberado() { iniciarBeep(1, 600,  0);  }
void somNegado()   { iniciarBeep(3, 150, 100); }
void somLeitura()  { iniciarBeep(2, 100, 100); }
