#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
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
const char* ssid     = "";
const char* password = "";
String serverUrl     = "https://ifaccess.iracema.xyz/acesso";
String matriculaUrl  = "https://ifaccess.iracema.xyz/matricula";

// ==========================
// ESTADO DA CATRACA
// ==========================
enum EstadoRotacao { IDLE, HORARIO, ANTIHORARIO };
EstadoRotacao estado = IDLE;

unsigned long lastCardRead  = 0;
const unsigned long releaseTime = 5000;
bool creditoRotacao = false;

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

  lcdIdle();
  noTone(BUZZER_PIN);
  Serial.println("Sistema iniciado");
}

// =====================================================
// LOOP
// =====================================================
void loop() {

  // ─────────────────────────────────────────────────
  // 0. TIMER LCD
  // ─────────────────────────────────────────────────
  if (lcdPendente && millis() - lcdTimer > 3000) {
    lcdIdle();
  }

  // ─────────────────────────────────────────────────
  // 1. TECLADO
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
  // 2. RFID
  // ─────────────────────────────────────────────────
  if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) {

    String uid = "";
    for (byte i = 0; i < mfrc522.uid.size; i++) {
      if (mfrc522.uid.uidByte[i] < 0x10) uid += "0";
      uid += String(mfrc522.uid.uidByte[i], HEX);
    }
    uid.toUpperCase();
    Serial.println("Cartão UID: " + uid);

    mostrarLCD("Lendo cartao...", uid);
    enviarUID(uid);

    mfrc522.PICC_HaltA();
    matriculaBuffer = "";
    aguardandoMatricula = false;
  }

  // ─────────────────────────────────────────────────
  // 3. CRÉDITO VÁLIDO?
  // ─────────────────────────────────────────────────
  bool liberado = creditoRotacao && (millis() - lastCardRead < releaseTime);
  if (!liberado) creditoRotacao = false;

  // ─────────────────────────────────────────────────
  // 4. SENSORES
  // ─────────────────────────────────────────────────
  bool esqAtivo = digitalRead(sensorEsq) == LOW;
  bool dirAtivo = digitalRead(sensorDir) == LOW;
  bool repouso  = esqAtivo && dirAtivo;

  // ─────────────────────────────────────────────────
  // 5. MÁQUINA DE ESTADOS
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
        } else {
          creditoRotacao = false;
          lastCardRead = 0;
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

  HTTPClient http;
  http.begin(serverUrl);
  http.addHeader("Content-Type", "application/json");

  String body = "{\"uid\":\"" + uid + "\"}";
  int code = http.POST(body);
  String resp = (code > 0) ? http.getString() : "";
  http.end();

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

  HTTPClient http;
  http.begin(matriculaUrl);
  http.addHeader("Content-Type", "application/json");

  String body = "{\"matricula\":\"" + matricula + "\"}";
  int code = http.POST(body);
  String resp = (code > 0) ? http.getString() : "";
  http.end();

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
// SONS — sem delay, tocam em background
// =====================================================
void somLiberado() {
  tone(BUZZER_PIN, 3000, 600);
}

void somNegado() {
  tone(BUZZER_PIN, 2500, 450);
}

void somLeitura() {
  tone(BUZZER_PIN, 2400, 300);
}