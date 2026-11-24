#include "firmware.h"
#include "driver/uart.h"

#define STM_RX     16          // RX2 <- A9
#define STM_TX     17          // TX2 -> A10
#define PIN_BOOT0  18          // -> broche du milieu de BOOT0
#define PIN_NRST   19          // -> R
#define ACK        0x79
#define FLASH_ADDR 0x08000000UL

HardwareSerial stm(2);

void resetStm(bool modeBootloader) {
  digitalWrite(PIN_BOOT0, modeBootloader ? HIGH : LOW);
  pinMode(PIN_NRST, OUTPUT);
  digitalWrite(PIN_NRST, LOW);     // reset actif
  delay(20);
  pinMode(PIN_NRST, INPUT);        // relâché
  delay(50);
}

int lireOctet(uint32_t delaiMs) {
  uint32_t debut = millis();
  while (millis() - debut < delaiMs) {
    if (stm.available()) return stm.read();
  }
  return -1;
}

bool attendreAck(uint32_t delaiMs) {
  int r = lireOctet(delaiMs);
  if (r == ACK) return true;
  Serial.printf(" [reponse : %d] ", r);
  return false;
}

bool envoyerCommande(uint8_t cmd) {
  stm.write(cmd);
  stm.write(cmd ^ 0xFF);
  return attendreAck(500);
}

bool envoyerAdresse(uint32_t adr) {
  uint8_t b[4] = { (uint8_t)(adr >> 24), (uint8_t)(adr >> 16),
                   (uint8_t)(adr >> 8),  (uint8_t)adr };
  stm.write(b, 4);
  stm.write(b[0] ^ b[1] ^ b[2] ^ b[3]);
  return attendreAck(500);
}

bool synchroniser() {
  for (int essai = 0; essai < 3; essai++) {
    resetStm(true);
    while (stm.available()) stm.read();
    stm.write(0x7F);
    if (lireOctet(500) == ACK) return true;
  }
  return false;
}

bool effacer() {
  if (envoyerCommande(0x43)) {
    stm.write(0xFF);
    stm.write(0x00);
    return attendreAck(20000);
  }
  if (envoyerCommande(0x44)) {
    stm.write(0xFF);
    stm.write(0xFF);
    stm.write(0x00);
    return attendreAck(20000);
  }
  return false;
}

bool ecrireBloc(uint32_t adr, const uint8_t *data, int n) {
  if (!envoyerCommande(0x31)) return false;
  if (!envoyerAdresse(adr)) return false;
  uint8_t cs = n - 1;
  stm.write((uint8_t)(n - 1));
  for (int i = 0; i < n; i++) {
    stm.write(data[i]);
    cs ^= data[i];
  }
  stm.write(cs);
  return attendreAck(1000);
}

bool lireBloc(uint32_t adr, uint8_t *buf, int n) {
  if (!envoyerCommande(0x11)) return false;
  if (!envoyerAdresse(adr)) return false;
  stm.write((uint8_t)(n - 1));
  stm.write((uint8_t)((n - 1) ^ 0xFF));
  if (!attendreAck(500)) return false;
  for (int i = 0; i < n; i++) {
    int c = lireOctet(500);
    if (c < 0) return false;
    buf[i] = c;
  }
  return true;
}

void arret() {
  Serial.println("Termine. Appuyez sur EN pour recommencer.");
  while (true) delay(1000);
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_BOOT0, OUTPUT);
  digitalWrite(PIN_BOOT0, LOW);
  pinMode(PIN_NRST, INPUT);
  stm.begin(115200, SERIAL_8E1, STM_RX, STM_TX);
  delay(1000);
  Serial.println();

  Serial.print("1. Bootloader... ");
  if (!synchroniser()) { Serial.println("ECHEC"); arret(); }
  Serial.println("OK");

  Serial.print("2. Effacement... ");
  if (!effacer()) { Serial.println("ECHEC"); arret(); }
  Serial.println("OK");

  Serial.printf("3. Ecriture de %u octets ", firmware_len);
  uint8_t bloc[256];
  for (uint32_t off = 0; off < firmware_len; off += 256) {
    uint32_t n = firmware_len - off;
    if (n > 256) n = 256;
    memset(bloc, 0xFF, sizeof(bloc));
    memcpy(bloc, firmware + off, n);
    int n4 = (n + 3) & ~3;
    if (!ecrireBloc(FLASH_ADDR + off, bloc, n4)) {
      Serial.printf("ECHEC a 0x%08lX\n", (unsigned long)(FLASH_ADDR + off));
      arret();
    }
    Serial.print(".");
  }
  Serial.println(" OK");

  Serial.print("4. Verification... ");
  uint8_t lu[256];
  for (uint32_t off = 0; off < firmware_len; off += 256) {
    uint32_t n = firmware_len - off;
    if (n > 256) n = 256;
    if (!lireBloc(FLASH_ADDR + off, lu, n) || memcmp(lu, firmware + off, n) != 0) {
      Serial.printf("ECHEC a 0x%08lX\n", (unsigned long)(FLASH_ADDR + off));
      arret();
    }
  }
  Serial.println("OK");

  Serial.println("5. Demarrage normal (BOOT0 = 0, reset)");
  uart_set_parity(UART_NUM_2, UART_PARITY_DISABLE);   // 8N1 avant le reset
  while (stm.available()) stm.read();
  resetStm(false);

  Serial.println("6. Sortie du programme :");
  String ligne = "";
  uint32_t debut = millis();
  bool fin = false;
  while (millis() - debut < 30000 && !fin) {
    if (stm.available()) {
      char c = stm.read();
      if (c == '\n') {
        ligne.trim();
        Serial.printf("   [%5.1f s] %s\n", (millis() - debut) / 1000.0, ligne.c_str());
        if (ligne == "FIN") fin = true;
        ligne = "";
      } else if (c != '\r') {
        ligne += c;
      }
    }
  }
  Serial.println(fin ? "Fin : message FIN recu" : "Fin : duree maximale atteinte");
  arret();
}

void loop() {}
