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
const char* ssid     = "Kauan ";
const char* password = "K240260n";
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
const unsigned long HTTP_TIMEOUT_MS = 3000;
const unsigned long MAX_REUSE_IDLE_MS = 4000;   // acima disso, proxy do servidor provavelmente ja fechou
const unsigned long STALE_WAIT_MS     = 1200;   // espera curta ao reaproveitar; se nao responder, refaz do zero

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

int fazerRequisicao(String url, String jsonBody, String &respostaOut) {
  String host, path;
  parseUrl(url, host, path);

  IPAddress ip;
  bool dnsOk = WiFi.hostByName(host.c_str(), ip);

  if (!secureClientPronto) {
    secureClient.setInsecure();
    secureClient.setTimeout(HTTP_TIMEOUT_MS / 1000);
    secureClient.setHandshakeTimeout(HTTP_TIMEOUT_MS);
    secureClientPronto = true;
  }

  bool reaproveitada = secureClient.connected() && secureClientHost == host &&
                        (millis() - secureClientLastUse < MAX_REUSE_IDLE_MS);
  bool connected = reaproveitada;
  if (!connected) {
    secureClient.stop();
    connected = dnsOk ? secureClient.connect(ip, 443, HTTP_TIMEOUT_MS) : secureClient.connect(host.c_str(), 443, HTTP_TIMEOUT_MS);
    if (connected) secureClientHost = host;
  }

  if (!connected) {
    secureClientHost = "";
    respostaOut = "";
    return -1;
  }

  String request = "POST " + path + " HTTP/1.1\r\n";
  request += "Host: " + host + "\r\n";
  request += "Content-Type: application/json\r\n";
  request += "Content-Length: " + String(jsonBody.length()) + "\r\n";
  request += "Connection: keep-alive\r\n\r\n";
  request += jsonBody;

  size_t escritos = secureClient.print(request);

  if (escritos == 0 && reaproveitada) {
    // conexao reaproveitada estava morta do lado do servidor — reconecta e tenta 1x
    secureClient.stop();
    connected = dnsOk ? secureClient.connect(ip, 443) : secureClient.connect(host.c_str(), 443);
    if (connected) {
      secureClientHost = host;
      escritos = secureClient.print(request);
    }
  }

  if (!connected || escritos == 0) {
    secureClient.stop();
    secureClientHost = "";
    respostaOut = "";
    return -1;
  }

  unsigned long esperaResposta = reaproveitada ? STALE_WAIT_MS : HTTP_TIMEOUT_MS;
  unsigned long limite = millis() + esperaResposta;
  while (!secureClient.available() && secureClient.connected() && millis() < limite) {
    delay(1);
  }

  if (reaproveitada && !secureClient.available()) {
    // conexao reaproveitada nao respondeu a tempo (provavelmente fechada em silencio
    // pelo proxy do servidor) — descarta e refaz com conexao nova, 1x
    secureClient.stop();
    connected = dnsOk ? secureClient.connect(ip, 443, HTTP_TIMEOUT_MS) : secureClient.connect(host.c_str(), 443, HTTP_TIMEOUT_MS);
    if (connected) {
      secureClientHost = host;
      escritos = secureClient.print(request);

      unsigned long limite2 = millis() + HTTP_TIMEOUT_MS;
      while (!secureClient.available() && secureClient.connected() && millis() < limite2) {
        delay(1);
      }
    }

    if (!connected || escritos == 0) {
      secureClient.stop();
      secureClientHost = "";
      respostaOut = "";
      return -1;
    }
  }

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

  if (fechaConexao) {
    secureClient.stop();
    secureClientHost = "";
  }

  if (secureClient.connected()) secureClientLastUse = millis();

  int code = 0;
  int sp1 = headers.indexOf(' ');
  if (headers.startsWith("HTTP/") && sp1 > 0) {
    int sp2 = headers.indexOf(' ', sp1 + 1);
    code = headers.substring(sp1 + 1, sp2).toInt();
  }

  respostaOut = corpo;
  return code;
}

// tenta 1x, e se falhar/der timeout, tenta mais 1x antes de desistir
int fazerRequisicaoComRetry(String url, String jsonBody, String &respostaOut) {
  int code = fazerRequisicao(url, jsonBody, respostaOut);
  if (code <= 0) {
    Serial.println("Falha na comunicacao — tentando novamente...");
    code = fazerRequisicao(url, jsonBody, respostaOut);
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
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);       // catraca fica na tomada — sem economia de energia, evita picos de latencia no radio
  WiFi.setAutoReconnect(true); // deixa o proprio driver do ESP32 reconectar assim que detecta a queda
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
  static unsigned long lastWifiTry = 0;
  if (WiFi.status() != WL_CONNECTED) {
    if (wifiCaiuEm == 0) wifiCaiuEm = millis();
    if (millis() - lastWifiTry > 2000) {
      lastWifiTry = millis();
      unsigned long caidoHa = millis() - wifiCaiuEm;
      if (caidoHa < 8000) {
        Serial.println("WiFi perdido — reconectando...");
        WiFi.reconnect();
      } else {
        Serial.println("WiFi ainda caido — reconexao completa...");
        WiFi.disconnect();
        WiFi.begin(ssid, password);
        wifiCaiuEm = millis(); // reinicia o ciclo: mais algumas tentativas leves antes do proximo begin()
      }
    }
  } else {
    wifiCaiuEm = 0;
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
    mostrarLCD("Sem WiFi!", "Acesso negado");
    somNegado();
    agendarLcdIdle();
    return;
  }

  String body = "{\"uid\":\"" + uid + "\"}";
  String resp;
  int code = fazerRequisicaoComRetry(serverUrl, body, resp);

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
  int code = fazerRequisicaoComRetry(matriculaUrl, body, resp);

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
