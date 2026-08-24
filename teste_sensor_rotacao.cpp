#include <Arduino.h>
// =====================================================
// Sentido de rotação de catraca — 2 sensores ópticos
// ESP32  |  Sensor 1 D0 → GPIO18  |  Sensor 2 D0 → GPIO19
// =====================================================
// Pinos do encoder
// Pinos
// ==========================
// CONFIGURAÇÃO DOS PINOS
// ==========================
const int pinA = 2; // D2
const int pinB = 4; // D4

// ==========================
// CONTADORES
// ==========================
volatile long pulsosHorario = 0;
volatile long pulsosAntiHorario = 0;

volatile long voltasHorario = 0;
volatile long voltasAntiHorario = 0;

// ==========================
// CONTROLE
// ==========================
volatile unsigned long ultimaLeitura = 0;
const int debounceMicros = 2000; // 2ms

volatile int ultimoEstadoA = 0;

// ==========================
// INTERRUPÇÃO (CANAL A)
// ==========================
void IRAM_ATTR handleEncoder() {
  unsigned long agora = micros();

  // Debounce (ignora ruído)
  if (agora - ultimaLeitura < debounceMicros) return;
  ultimaLeitura = agora;

  int estadoA = digitalRead(pinA);
  int estadoB = digitalRead(pinB);

  // Detecta borda de subida (0 -> 1)
  if (estadoA == HIGH && ultimoEstadoA == LOW) {

    // Log dos sensores
    Serial.print("A: ");
    Serial.print(estadoA);
    Serial.print(" | B: ");
    Serial.println(estadoB);

    // Determina sentido
    if (estadoB == LOW) {
      pulsosHorario++;
      Serial.println("➡️ Sentido: Horário");
    } else {
      pulsosAntiHorario++;
      Serial.println("⬅️ Sentido: Anti-horário");
    }

    // Calcula voltas (3 pulsos = 1 volta)
    voltasHorario = pulsosHorario / 3;
    voltasAntiHorario = pulsosAntiHorario / 3;
  }

  ultimoEstadoA = estadoA;
}

// ==========================
// SETUP
// ==========================
void setup() {
  Serial.begin(115200);

  pinMode(pinA, INPUT_PULLUP);
  pinMode(pinB, INPUT_PULLUP);

  ultimoEstadoA = digitalRead(pinA);

  attachInterrupt(digitalPinToInterrupt(pinA), handleEncoder, CHANGE);

  Serial.println("Sistema pronto (modo robusto para catraca 3 dentes)");
}

// ==========================
// LOOP
// ==========================
void loop() {
  static unsigned long tempo = 0;

  if (millis() - tempo > 1000) {
    tempo = millis();

    Serial.println("========== STATUS ==========");

    Serial.print("Pulsos Horário: ");
    Serial.println(pulsosHorario);

    Serial.print("Pulsos Anti-horário: ");
    Serial.println(pulsosAntiHorario);

    Serial.print("Voltas Horário: ");
    Serial.println(voltasHorario);

    Serial.print("Voltas Anti-horário: ");
    Serial.println(voltasAntiHorario);
  }
}