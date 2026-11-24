#include <WiFi.h>
#include <WiFiMulti.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include "mbedtls/sha256.h"
#include "driver/uart.h"
#include "secrets.h"

#define STM_RX        16          // RX2 <- A9
#define STM_TX        17          // TX2 -> A10
#define PIN_BOOT0     18          // -> milieu de BOOT0
#define PIN_NRST      19          // -> R
#define ACK           0x79
#define FLASH_ADDR    0x08000000UL
#define FW_PATH       "/fw.bin"
#define DUREE_MAX_MS  30000       // durée maximale d'exécution
#define INTERVALLE_MS 5000        // intervalle entre deux demandes au serveur

HardwareSerial stm(2);
WiFiMulti wifi;

// ================= Bootloader STM32 =================

void resetStm(bool modeBootloader) {
  digitalWrite(PIN_BOOT0, modeBootloader ? HIGH : LOW);
  pinMode(PIN_NRST, OUTPUT);
  digitalWrite(PIN_NRST, LOW);
  delay(20);
  pinMode(PIN_NRST, INPUT);
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
  return lireOctet(delaiMs) == ACK;
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
  uart_set_parity(UART_NUM_2, UART_PARITY_EVEN);     // le bootloader parle en 8E1
  for (int essai = 0; essai < 3; essai++) {
    resetStm(true);
    while (stm.available()) stm.read();
    stm.write(0x7F);
    if (lireOctet(500) == ACK) return true;
  }
  return false;
}

String lireIdPuce() {
  if (!envoyerCommande(0x02)) return "";
  int n = lireOctet(500);
  uint16_t id = 0;
  for (int i = 0; i <= n; i++) id = (id << 8) | lireOctet(500);
  lireOctet(500);
  char buf[8];
  snprintf(buf, sizeof(buf), "0x%04X", id);
  return String(buf);
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

// ================= Programmation et exécution =================

bool programmer(uint32_t taille, String &puce, String &erreur) {
  if (!synchroniser()) { erreur = "pas de reponse du bootloader"; return false; }
  puce = lireIdPuce();
  Serial.printf("   Puce %s\n", puce.c_str());
  if (!effacer()) { erreur = "effacement refuse"; return false; }

  File f = LittleFS.open(FW_PATH, "r");
  if (!f) { erreur = "fichier local introuvable"; return false; }
  uint8_t bloc[256], lu[256];

  for (uint32_t off = 0; off < taille; off += 256) {     // écriture
    uint32_t n = std::min(taille - off, (uint32_t)256);
    memset(bloc, 0xFF, sizeof(bloc));
    f.read(bloc, n);
    if (!ecrireBloc(FLASH_ADDR + off, bloc, (n + 3) & ~3)) {
      f.close();
      erreur = "ecriture refusee a 0x" + String(FLASH_ADDR + off, HEX);
      return false;
    }
  }
  Serial.println("   Ecriture OK");

  f.seek(0);
  for (uint32_t off = 0; off < taille; off += 256) {     // vérification
    uint32_t n = std::min(taille - off, (uint32_t)256);
    f.read(bloc, n);
    if (!lireBloc(FLASH_ADDR + off, lu, n) || memcmp(lu, bloc, n) != 0) {
      f.close();
      erreur = "verification echouee a 0x" + String(FLASH_ADDR + off, HEX);
      return false;
    }
  }
  f.close();
  Serial.println("   Verification OK");
  return true;
}

String executer(String &sortie) {
  uart_set_parity(UART_NUM_2, UART_PARITY_DISABLE);  // le programme parle en 8N1
  while (stm.available()) stm.read();
  resetStm(false);                                   // démarrage normal

  String ligne;
  uint32_t debut = millis();
  while (millis() - debut < DUREE_MAX_MS) {
    if (!stm.available()) continue;
    char c = stm.read();
    if (c == '\n') {
      ligne.trim();
      Serial.printf("   | %s\n", ligne.c_str());
      sortie += ligne + "\n";
      if (ligne == "FIN") return "FIN";
      ligne = "";
      if (sortie.length() > 15000) return "sortie_trop_longue";
    } else if (c != '\r') {
      ligne += c;
    }
  }
  if (ligne.length()) sortie += ligne;
  return "duree_max";
}

// ================= Échanges avec le serveur =================

String champJson(const String &json, const char *cle) {
  String motif = String("\"") + cle + "\":";
  int i = json.indexOf(motif);
  if (i < 0) return "";
  i += motif.length();
  while (i < (int)json.length() && json[i] == ' ') i++;
  if (json[i] == '"') return json.substring(i + 1, json.indexOf('"', i + 1));
  int fin = i;
  while (fin < (int)json.length() && isDigit(json[fin])) fin++;
  return json.substring(i, fin);
}

String echapperJson(const String &s) {
  String r;
  r.reserve(s.length() + 16);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') { r += '\\'; r += c; }
    else if (c == '\n') r += "\\n";
    else if ((uint8_t)c < 0x20 || (uint8_t)c >= 0x7F) r += '?';
    else r += c;
  }
  return r;
}

bool telecharger(const String &id, uint32_t taille, const String &shaAttendu) {
  HTTPClient http;
  http.begin(String(API_URL) + "/agent/jobs/" + id + "/firmware");
  http.addHeader("X-Agent-Key", AGENT_KEY);
  if (http.GET() != 200) { http.end(); return false; }

  File f = LittleFS.open(FW_PATH, "w");
  if (!f) { http.end(); return false; }
  WiFiClient *flux = http.getStreamPtr();

  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);

  uint8_t buf[512];
  uint32_t recu = 0, dernier = millis();
  while (recu < taille && millis() - dernier < 10000) {
    size_t dispo = flux->available();
    if (dispo) {
      size_t n = flux->readBytes(buf, std::min(dispo, sizeof(buf)));
      f.write(buf, n);
      mbedtls_sha256_update(&ctx, buf, n);
      recu += n;
      dernier = millis();
    } else {
      delay(1);
    }
  }
  f.close();
  http.end();

  uint8_t hash[32];
  mbedtls_sha256_finish(&ctx, hash);
  mbedtls_sha256_free(&ctx);
  char hex[65];
  for (int i = 0; i < 32; i++) sprintf(hex + 2 * i, "%02x", hash[i]);

  bool ok = (recu == taille) && shaAttendu.equalsIgnoreCase(hex);
  Serial.printf("   Telechargement : %u octets, empreinte %s\n", recu, ok ? "OK" : "INVALIDE");
  return ok;
}

void envoyerResultat(const String &id, bool succes, const String &puce,
                     const String &sortie, const String &fin, const String &erreur) {
  String corps = String("{\"succes\":") + (succes ? "true" : "false") +
                 ",\"chip_id\":\"" + puce + "\"" +
                 ",\"output\":\"" + echapperJson(sortie) + "\"" +
                 ",\"end_reason\":\"" + fin + "\"" +
                 ",\"error\":" + (erreur.length() ? "\"" + echapperJson(erreur) + "\"" : String("null")) +
                 "}";
  HTTPClient http;
  http.begin(String(API_URL) + "/agent/jobs/" + id + "/result");
  http.addHeader("X-Agent-Key", AGENT_KEY);
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(corps);
  http.end();
  Serial.printf("   Resultat envoye au serveur (code %d)\n", code);
}

void traiterTravail(const String &json) {
  String id = champJson(json, "id");
  uint32_t taille = champJson(json, "size").toInt();
  String sha = champJson(json, "sha256");
  Serial.printf("\nTravail %s : %u octets\n", id.c_str(), taille);

  String puce, sortie, erreur, fin;
  bool ok = false;
  if (!telecharger(id, taille, sha)) {
    erreur = "telechargement incomplet ou empreinte invalide";
  } else if (programmer(taille, puce, erreur)) {
    fin = executer(sortie);
    ok = true;
  }
  Serial.printf("   Bilan : %s %s\n", ok ? "termine" : "echec", erreur.c_str());
  envoyerResultat(id, ok, puce, sortie, fin, erreur);
}

// ================= Programme principal =================

void setup() {
  Serial.begin(115200);
  pinMode(PIN_BOOT0, OUTPUT);
  digitalWrite(PIN_BOOT0, LOW);
  pinMode(PIN_NRST, INPUT);
  stm.begin(115200, SERIAL_8E1, STM_RX, STM_TX);
  delay(1000);
  Serial.println("\n=== Passerelle STM32 ===");

  if (!LittleFS.begin(true)) Serial.println("Erreur : systeme de fichiers indisponible");

  wifi.addAP(WIFI_SSID_1, WIFI_PASS_1);
  if (strlen(WIFI_SSID_2) > 0) wifi.addAP(WIFI_SSID_2, WIFI_PASS_2);
  Serial.print("Connexion Wi-Fi");
  while (wifi.run() != WL_CONNECTED) {
    Serial.print(".");
    delay(500);
  }
  Serial.printf("\nConnecte a %s, adresse %s\n",
                WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
  Serial.println("En attente de travaux");
}

void loop() {
  if (wifi.run() != WL_CONNECTED) {
    Serial.println("Wi-Fi perdu, reconnexion...");
    delay(1000);
    return;
  }

  HTTPClient http;
  http.begin(String(API_URL) + "/agent/next");
  http.addHeader("X-Agent-Key", AGENT_KEY);
  int code = http.GET();

  if (code == 200) {
    String json = http.getString();
    http.end();
    traiterTravail(json);
  } else {
    http.end();
    if (code == 204) Serial.print(".");          // rien à faire
    else Serial.printf("\nErreur serveur : %d\n", code);
  }
  delay(INTERVALLE_MS);
}