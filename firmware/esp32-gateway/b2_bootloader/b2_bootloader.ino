#define STM_RX    16   // RX2 de l'ESP32  <- A9 de la Blue Pill
#define STM_TX    17   // TX2 de l'ESP32  -> A10 de la Blue Pill
#define ACK  0x79
#define NACK 0x1F

HardwareSerial stm(2);

int lireOctet(uint32_t delaiMs) {
  uint32_t debut = millis();
  while (millis() - debut < delaiMs) {
    if (stm.available()) return stm.read();
  }
  return -1;
}

bool envoyerCommande(uint8_t cmd) {
  stm.write(cmd);
  stm.write(cmd ^ 0xFF);
  return lireOctet(500) == ACK;
}

void setup() {
  Serial.begin(115200);
  stm.begin(115200, SERIAL_8E1, STM_RX, STM_TX);
  delay(1000);
  Serial.println();
  Serial.println("Pret. Appuyez sur RESET de la Blue Pill.");
}

void loop() {
  while (stm.available()) stm.read();   // vide les parasites

  stm.write(0x7F);
  int r = lireOctet(500);

  if (r == ACK) {
    Serial.println("Bootloader STM32 : OK");
    if (envoyerCommande(0x02)) {
      int n = lireOctet(500);
      uint16_t id = 0;
      for (int i = 0; i <= n; i++) id = (id << 8) | lireOctet(500);
      lireOctet(500);
      Serial.printf("Identifiant de la puce : 0x%04X\n", id);
    }
    Serial.println("Test termine.");
    while (true) delay(1000);           // on s'arrête ici
  }

  if (r < 0) Serial.println("Reponse : rien");
  else       Serial.printf("Reponse : 0x%02X\n", r);

  delay(2000);
}