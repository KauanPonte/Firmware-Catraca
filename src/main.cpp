#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
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
const char* ssid     = "IFCE-IOT";
const char* password = "1fiot24c3";
String serverUrl     = "https://ifaccess.iracema.xyz/acesso";
String matriculaUrl  = "https://ifaccess.iracema.xyz/matricula";

// ==========================
// REQUISICAO HTTPS MANUAL
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
unsigned long secureClientLastUse = 0;
IPAddress serverIp;              // DNS em cache: resolver a cada cartao custava tempo a toa
String serverIpHost = "";
const unsigned long HTTP_TIMEOUT_MS   = 4000;
const uint32_t      CONNECT_TIMEOUT_S = 5;      // handshake TLS no ESP32 leva ~1-2s; 3s dava falso timeout
const unsigned long MAX_REUSE_IDLE_MS = 4000;   // servidor (Node) fecha conexao ociosa com 5s
const unsigned long STALE_WAIT_MS     = 1200;   // espera curta ao reaproveitar; se nao responder, refaz do zero
const unsigned long KEEPALIVE_MS      = 3000;   // ping pra manter a conexao TLS aberta e o cartao nao pagar o handshake
const unsigned long KEEPALIVE_FALHA_MS = 30000; // se o ping falhar, espera mais pra nao travar o loop tentando

// tempo da ultima validacao no LCD (ex: "OK 185ms reap") — false volta ao LCD normal
const bool MOSTRAR_TEMPO_LCD = false;
String        ultimaConexao = "";  // reap / nova / morta / falha / 2x
unsigned long ultimoTempoMs = 0;

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

void fecharConexao() {
  secureClient.stop();
  secureClientHost = "";
}

bool abrirConexao(const String &host) {
  fecharConexao();
  if (serverIpHost != host) {
    if (!WiFi.hostByName(host.c_str(), serverIp)) return false;
    serverIpHost = host;
  }
  // passa o host junto do IP: connect(ip) sozinho manda o IP como SNI no TLS
  if (!secureClient.connect(serverIp, 443, host.c_str(), NULL, NULL, NULL)) {
    serverIpHost = ""; // talvez o IP tenha mudado — resolve de novo na proxima
    return false;
  }
  secureClientHost = host;
  return true;
}

// envia a requisicao na conexao ja aberta e le a resposta.
// retorna -1 se nada chegou dentro de esperaMs (conexao morta)
int transacao(const String &request, bool semCorpo, unsigned long esperaMs, String &corpo) {
  corpo = "";
  if (secureClient.print(request) == 0) { fecharConexao(); return -1; }

  unsigned long inicio = millis();
  while (!secureClient.available() && secureClient.connected() && millis() - inicio < esperaMs) {
    delay(1);
  }
  if (!secureClient.available()) { fecharConexao(); return -1; }

  String headers = "";
  bool headersCompletos = false;
  inicio = millis();
  while (!headersCompletos && millis() - inicio < HTTP_TIMEOUT_MS) {
    while (secureClient.available()) {
      headers += (char)secureClient.read();
      if (headers.endsWith("\r\n\r\n")) { headersCompletos = true; break; }
    }
    if (!headersCompletos && !secureClient.connected() && !secureClient.available()) break;
    if (!headersCompletos) delay(1);
  }

  int code = 0;
  int sp1 = headers.indexOf(' ');
  if (headers.startsWith("HTTP/") && sp1 > 0) {
    int sp2 = headers.indexOf(' ', sp1 + 1);
    code = headers.substring(sp1 + 1, sp2).toInt();
  }

  int contentLength = extrairContentLength(headers);
  bool chunked = headers.indexOf("Transfer-Encoding: chunked") >= 0 ||
                 headers.indexOf("transfer-encoding: chunked") >= 0;

  // so mantem a conexao se a resposta terminou de forma bem delimitada
  bool completo = headersCompletos;
  if (completo && !semCorpo) {
    inicio = millis();
    if (contentLength >= 0) {
      while ((int)corpo.length() < contentLength && millis() - inicio < HTTP_TIMEOUT_MS) {
        while (secureClient.available() && (int)corpo.length() < contentLength) corpo += (char)secureClient.read();
        if (!secureClient.connected() && !secureClient.available()) break;
        if ((int)corpo.length() < contentLength) delay(1);
      }
      completo = (int)corpo.length() >= contentLength;
    } else if (chunked) {
      // antes esperava o servidor fechar (que nunca fecha em keep-alive) = 3s perdidos
      while (!corpo.endsWith("0\r\n\r\n") && millis() - inicio < HTTP_TIMEOUT_MS) {
        while (secureClient.available()) corpo += (char)secureClient.read();
        if (!secureClient.connected() && !secureClient.available()) break;
        if (!corpo.endsWith("0\r\n\r\n")) delay(1);
      }
      completo = corpo.endsWith("0\r\n\r\n");
    } else {
      while ((secureClient.connected() || secureClient.available()) && millis() - inicio < HTTP_TIMEOUT_MS) {
        while (secureClient.available()) corpo += (char)secureClient.read();
        delay(1);
      }
      completo = false;
    }
  }

  if (!completo || respostaFechaConexao(headers)) fecharConexao();
  else secureClientLastUse = millis();

  return code;
}

int fazerRequisicao(String metodo, String url, String jsonBody, String &respostaOut) {
  String host, path;
  parseUrl(url, host, path);

  if (!secureClientPronto) {
    secureClient.setInsecure();
    secureClient.setTimeout(CONNECT_TIMEOUT_S);          // em segundos
    secureClient.setHandshakeTimeout(CONNECT_TIMEOUT_S); // em segundos (antes recebia 3000 = 50 min travado)
    secureClientPronto = true;
  }

  bool reaproveitada = secureClient.connected() && secureClientHost == host &&
                        (millis() - secureClientLastUse < MAX_REUSE_IDLE_MS);
  unsigned long t0 = millis();
  if (!reaproveitada && !abrirConexao(host)) {
    Serial.printf("[%s] falha ao conectar apos %lu ms\n", metodo.c_str(), millis() - t0);
    if (metodo != "HEAD") ultimaConexao = "falha";
    respostaOut = "";
    return -1;
  }
  unsigned long tConexao = millis() - t0;

  String request = metodo + " " + path + " HTTP/1.1\r\n";
  request += "Host: " + host + "\r\n";
  if (jsonBody.length() > 0) {
    request += "Content-Type: application/json\r\n";
    request += "Content-Length: " + String(jsonBody.length()) + "\r\n";
  }
  request += "Connection: keep-alive\r\n\r\n";
  request += jsonBody;

  bool semCorpo = (metodo == "HEAD");
  unsigned long t1 = millis();
  int code = transacao(request, semCorpo, reaproveitada ? STALE_WAIT_MS : HTTP_TIMEOUT_MS, respostaOut);
  unsigned long tResposta = millis() - t1;

  bool refeita = false;
  if (code < 0 && reaproveitada) {
    // conexao reaproveitada tinha sido fechada em silencio pelo servidor — refaz 1x
    refeita = true;
    t0 = millis();
    if (!abrirConexao(host)) {
      if (!semCorpo) ultimaConexao = "falha";
      respostaOut = "";
      return -1;
    }
    tConexao = millis() - t0;
    t1 = millis();
    code = transacao(request, semCorpo, HTTP_TIMEOUT_MS, respostaOut);
    tResposta = millis() - t1;
  }

  if (!semCorpo) ultimaConexao = refeita ? "morta" : (reaproveitada ? "reap" : "nova");

  // HEAD = ping de manutencao; so loga se algo der errado pra nao poluir o Serial
  if (!semCorpo || code <= 0) {
    Serial.printf("[%s %s] conexao: %s %lu ms | resposta: %lu ms | HTTP %d\n",
                  metodo.c_str(), path.c_str(),
                  refeita ? "MORTA, refeita" : (reaproveitada ? "reaproveitada" : "nova"),
                  tConexao, tResposta, code);
  }
  return code;
}

int fazerRequisicao(String url, String jsonBody, String &respostaOut) {
  return fazerRequisicao("POST", url, jsonBody, respostaOut);
}

// mantem a conexao TLS aberta enquanto a catraca esta parada,
// pra leitura do cartao nao pagar DNS + handshake (~1-2s no ESP32)
void manterConexaoAquecida() {
  static unsigned long proximoPing = 0;
  if ((long)(millis() - proximoPing) < 0) return;
  if (secureClient.connected() && millis() - secureClientLastUse < KEEPALIVE_MS) return;

  String host, path, resp;
  parseUrl(serverUrl, host, path);
  int code = fazerRequisicao("HEAD", "https://" + host + "/", "", resp);
  proximoPing = millis() + (code > 0 ? KEEPALIVE_MS : KEEPALIVE_FALHA_MS);
}

// tenta 1x, e se falhar/der timeout, tenta mais 1x antes de desistir
int fazerRequisicaoComRetry(String url, String jsonBody, String &respostaOut) {
  int code = fazerRequisicao(url, jsonBody, respostaOut);
  if (code <= 0) {
    Serial.println("Falha na comunicacao — tentando novamente...");
    code = fazerRequisicao(url, jsonBody, respostaOut);
    if (code > 0) ultimaConexao = "2x"; // so funcionou na segunda tentativa
  }
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
bool modoLivre = false; // catraca sempre liberada — usar se a internet do IF cair
String lastUID = "";

// ==========================
// ESTADO DO TECLADO
// ==========================
String matriculaBuffer = "";
bool aguardandoMatricula = false;

// ==========================
// MENU ADMIN (TECLA D = F4)
// ==========================
const String SENHA_ADMIN = "9635"; // PIN numerico de 4 digitos para entrar no menu admin

enum ModoAdmin { ADMIN_OFF, ADMIN_SENHA, ADMIN_MENU };
ModoAdmin modoAdmin = ADMIN_OFF;
String adminBuffer = "";

unsigned long wifiCaiuEm = 0;
const unsigned long WIFI_RECONEXAO_MS = 20000; // tempo que o driver tem pra reconectar sozinho antes de um begin() novo

// ==========================
// DIAGNOSTICO WIFI
// ==========================
volatile uint8_t wifiUltimoMotivo = 0;   // codigo de desconexao do driver (wifi_err_reason_t)
volatile bool    wifiAssociado    = false; // ligado no roteador mas ainda sem IP

void eventoWiFi(WiFiEvent_t event, WiFiEventInfo_t info) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      wifiAssociado = true;
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      wifiUltimoMotivo = 0;
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      wifiAssociado = false;
      wifiUltimoMotivo = info.wifi_sta_disconnected.reason;
      break;
    default:
      break;
  }
}

// texto curto (cabe no LCD) explicando por que nao conectou
String motivoWiFi() {
  if (WiFi.status() == WL_CONNECTED) return "Conectado";
  if (wifiAssociado)                 return "Sem IP (DHCP)";
  switch (wifiUltimoMotivo) {
    case 0:   return "Conectando...";
    case 201: return "Rede nao achada";   // NO_AP_FOUND: SSID errado, fora de alcance ou so 5GHz
    case 15:
    case 204: return "Senha errada?";     // 4WAY_HANDSHAKE_TIMEOUT / HANDSHAKE_TIMEOUT
    case 202: return "Auth recusada";     // AUTH_FAIL: senha ou bloqueio por MAC
    case 203: return "Assoc recusada";    // ASSOC_FAIL: roteador recusou (MAC/lotado)
    case 200: return "Sinal fraco";       // BEACON_TIMEOUT
    default:  return "Erro " + String(wifiUltimoMotivo);
  }
}

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
void    forcarReconexaoWiFi();

void forcarReconexaoWiFi() {
  mostrarLCD("Reconectando", "WiFi, aguarde...");
  Serial.println("Reconexao forcada pelo teclado");
  WiFi.disconnect();
  delay(100);
  WiFi.begin(ssid, password);
  wifiCaiuEm = millis();
  somLeitura();
  agendarLcdIdle();
}

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
  WiFi.onEvent(eventoWiFi);
  WiFi.mode(WIFI_STA);
  Serial.println("MAC (pra liberar na rede): " + WiFi.macAddress());
  WiFi.setSleep(false);       // catraca fica na tomada — sem economia de energia, evita picos de latencia no radio
  WiFi.setAutoReconnect(true); // deixa o proprio driver do ESP32 reconectar assim que detecta a queda
  WiFi.disconnect();
  delay(100);
  WiFi.begin(ssid, password);

  int tentativas = 0;
  while (WiFi.status() != WL_CONNECTED && tentativas < 40) {
    delay(500);
    Serial.print(".");
    tentativas++;
  }

  if (WiFi.status() != WL_CONNECTED) {
    String motivo = motivoWiFi();
    Serial.println("Falha no WiFi: " + motivo + " (codigo " + String(wifiUltimoMotivo) + ")");

    // procura a rede pra saber se o ESP ao menos enxerga ela (ESP32 so pega 2.4GHz)
    int n = WiFi.scanNetworks();
    int rssi = 0;
    for (int i = 0; i < n; i++) {
      if (WiFi.SSID(i) == ssid) { rssi = WiFi.RSSI(i); break; }
    }
    WiFi.scanDelete();
    if (rssi == 0) Serial.printf("Rede \"%s\" NAO encontrada no scan (%d redes vistas)\n", ssid, n);
    else           Serial.printf("Rede \"%s\" encontrada, sinal %d dBm\n", ssid, rssi);

    mostrarLCD("Sem WiFi!", motivo);
    delay(3000);
    WiFi.begin(ssid, password); // o scan interrompe a tentativa; o loop continua daqui
    wifiCaiuEm = millis();
  } else {
    Serial.println("WiFi conectado! IP: " + WiFi.localIP().toString());
    mostrarLCD("WiFi OK!", WiFi.localIP().toString());
    delay(1500);
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
  // 0. BUZZER
  // ─────────────────────────────────────────────────
  processarBeep();

  // ─────────────────────────────────────────────────
  // 1. WIFI — reconexao ativa (nao depende so do
  //    auto-reconnect do driver, que as vezes nao volta)
  // ─────────────────────────────────────────────────
  static bool estavaConectado = false;
  if ((WiFi.status() == WL_CONNECTED) != estavaConectado) {
    estavaConectado = !estavaConectado;
    if (estavaConectado) Serial.println("WiFi conectado! IP: " + WiFi.localIP().toString());
    else                 Serial.println("WiFi caiu: " + motivoWiFi());
  }

  // antes chamava reconnect() a cada 2s, o que interrompia a tentativa em
  // andamento (auth + DHCP na rede do IF pode passar de 2s) e nunca conectava
  if (WiFi.status() != WL_CONNECTED) {
    if (wifiCaiuEm == 0) wifiCaiuEm = millis();
    if (millis() - wifiCaiuEm > WIFI_RECONEXAO_MS) {
      Serial.printf("WiFi caido ha %lus (%s) — reconexao completa...\n",
                    (millis() - wifiCaiuEm) / 1000, motivoWiFi().c_str());
      WiFi.disconnect();
      WiFi.begin(ssid, password);
      wifiCaiuEm = millis();
    }
  } else {
    wifiCaiuEm = 0;
    if (estado == IDLE && !modoLivre) manterConexaoAquecida();
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
    Serial.println(String("Tecla: ") + key);

    if (modoAdmin == ADMIN_SENHA) {
      // ---- digitando a senha pra entrar no menu admin ----
      if (key == '*') {
        modoAdmin = ADMIN_OFF;
        adminBuffer = "";
        lcdIdle();
      } else if (key >= '0' && key <= '9') {
        adminBuffer += key;
        lcd.setCursor(0, 1);
        String mascara = "";
        for (uint8_t i = 0; i < adminBuffer.length(); i++) mascara += "*";
        lcd.print(mascara + "          ");

        if (adminBuffer.length() >= 4) {
          if (adminBuffer == SENHA_ADMIN) {
            modoAdmin = ADMIN_MENU;
            mostrarLCD("A:Reset B:Wifi", "C:Livre   *:Sai");
            somLeitura();
          } else {
            modoAdmin = ADMIN_OFF;
            mostrarLCD("Senha invalida", "");
            somNegado();
            agendarLcdIdle();
          }
          adminBuffer = "";
        }
      }

    } else if (modoAdmin == ADMIN_MENU) {
      // ---- menu admin: A reinicia, B forca reconexao WiFi, C alterna acesso livre ----
      if (key == 'A') {
        mostrarLCD("Reiniciando...", "");
        somLeitura();
        delay(300);
        ESP.restart();
      } else if (key == 'B') {
        modoAdmin = ADMIN_OFF;
        forcarReconexaoWiFi();
      } else if (key == 'C') {
        modoLivre = !modoLivre;
        modoAdmin = ADMIN_OFF;
        if (modoLivre) {
          mostrarLCD("Acesso Livre", "ATIVADO");
        } else {
          mostrarLCD("Acesso Livre", "desativado");
        }
        somLiberado();
        agendarLcdIdle();
      } else if (key == '*') {
        modoAdmin = ADMIN_OFF;
        lcdIdle();
      }

    } else if (key == 'D') {
      // ---- F4: entra no menu admin, pede senha ----
      matriculaBuffer = "";
      aguardandoMatricula = false;
      modoAdmin = ADMIN_SENHA;
      adminBuffer = "";
      mostrarLCD("Senha admin:", "");

    } else if (key == '#') {
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
      enviarUID(uid);
      matriculaBuffer = "";
      aguardandoMatricula = false;
    }
  }

  // ─────────────────────────────────────────────────
  // 5. CRÉDITO VÁLIDO?
  // ─────────────────────────────────────────────────
  bool liberado = modoLivre || (creditoRotacao && (millis() - lastCardRead < releaseTime));
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
    mostrarLCD("Sem WiFi!", motivoWiFi());
    somNegado();
    agendarLcdIdle();
    return;
  }

  String body = "{\"uid\":\"" + uid + "\"}";
  String resp;
  unsigned long t0 = millis();
  int code = fazerRequisicaoComRetry(serverUrl, body, resp);
  ultimoTempoMs = millis() - t0;

  Serial.printf("RFID: HTTP %d em %lu ms\n", code, ultimoTempoMs);
  Serial.println("Resp RFID: " + resp);
  processarResposta(code, resp);
}

// =====================================================
// ENVIA MATRÍCULA (TECLADO)
// =====================================================
void enviarMatricula(String matricula) {
  if (WiFi.status() != WL_CONNECTED) {
    mostrarLCD("Sem WiFi!", motivoWiFi());
    somNegado();
    agendarLcdIdle();
    return;
  }

  String body = "{\"matricula\":\"" + matricula + "\"}";
  String resp;
  unsigned long t0 = millis();
  int code = fazerRequisicaoComRetry(matriculaUrl, body, resp);
  ultimoTempoMs = millis() - t0;

  Serial.printf("Matricula: HTTP %d em %lu ms\n", code, ultimoTempoMs);
  Serial.println("Resp Matricula: " + resp);
  processarResposta(code, resp);
}

// =====================================================
// PROCESSA RESPOSTA
// =====================================================
// linha do LCD com resultado + tempo + tipo de conexao, ex: "OK 185ms reap"
String linhaTempo(String tag) {
  String ms = ultimoTempoMs < 10000 ? String(ultimoTempoMs) + "ms" : String(ultimoTempoMs / 1000) + "s";
  String s = tag + " " + ms + " " + ultimaConexao;
  return s.substring(0, 16);
}

void processarResposta(int httpCode, String response) {
  if (httpCode <= 0) {
    mostrarLCD("Erro servidor", MOSTRAR_TEMPO_LCD ? linhaTempo("ERRO") : "Acesso negado");
    somNegado();
    agendarLcdIdle();
    return;
  }

  if (response.indexOf("\"resultado\":\"leitura\"") >= 0) {
    mostrarLCD("Modo cadastro", MOSTRAR_TEMPO_LCD ? linhaTempo("UID") : "UID capturado");
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
    if (MOSTRAR_TEMPO_LCD) l2 = linhaTempo("OK");
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
  if (modoLivre) {
    mostrarLCD("Acesso Livre", "catraca liberada");
  } else {
    mostrarLCD("Aproxime cartao", "ou matricula:#");
  }
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
