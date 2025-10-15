// ===== ESP01_Worker_ESPNow_Slave_FINAL.ino =====
// Board: Generic ESP8266 (ESP-01)
// Tools -> CPU Frequency: 160 MHz (recommended for speed)
// Serial Monitor: 115200

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <espnow.h>
#include <user_interface.h>

#include "Counter.h"
#include "DSHA1.h"

#ifndef LED_BUILTIN
#define LED_BUILTIN 2
#endif

// ===== USER CONFIG =====
static const char RIG_IDENTIFIER[] PROGMEM = "None"; // atau "None"

static const uint8_t WIFI_CHANNEL = 1;  // samakan dgn channel AP router/gateway
static const uint8_t BCAST_MAC[6] = { 255, 255, 255, 255, 255, 255 };

// ===== State =====
volatile bool got = false;
char rx[240];
uint8_t lastSrc[6];

bool paired = false;
uint8_t DEST_MAC[6] = { 0 };

String NODE_ID;   // ex: "D4F8A9"
String RIG_NAME;  // ex: "ESP01S-D4F8A9" atau "<RIG_IDENTIFIER>-D4F8A9"
String CHIP_STR;  // ex: "d4f8a9"

DSHA1 dsha1;
uint8_t job_expected[20];
String job_last_hash;
unsigned int job_targets = 0;
char g_jobTag[9] = { 0 };  // 8 hex tail dari lastHash

// ---------- utils ----------
static inline void printMac(const uint8_t *m) {
  Serial.printf("%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
}
static inline uint8_t hexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return 0;
}
static void hexToBytes40(const char *s, uint8_t out20[20]) {
  for (int i = 0; i < 20; i++) {
    uint8_t hi = hexNibble(s[2 * i]), lo = hexNibble(s[2 * i + 1]);
    out20[i] = (hi << 4) | lo;
  }
}
static String chipHexUpper() {
  uint8_t mac[6];
  wifi_get_macaddr(STATION_IF, mac);
  char nid[7];
  sprintf(nid, "%02X%02X%02X", mac[3], mac[4], mac[5]);
  return String(nid);
}
static String chipHexLower() {
  uint8_t mac[6];
  wifi_get_macaddr(STATION_IF, mac);
  char nid[7];
  sprintf(nid, "%02x%02x%02x", mac[3], mac[4], mac[5]);
  return String(nid);
}

// ---------- callbacks ----------
void onRecv(uint8_t *mac, uint8_t *data, uint8_t len) {
  int n = min((int)len, (int)sizeof(rx) - 1);
  memcpy(lastSrc, mac, 6);
  memcpy(rx, data, n);
  rx[n] = '\0';
  got = true;
}
void onSent(uint8_t *mac, uint8_t status) {
  // diamkan log broadcast (biar bersih)
  if (mac && memcmp(mac, BCAST_MAC, 6) == 0) return;
  Serial.print("[TX sent ");
  if (mac) printMac(mac);
  else Serial.print("BCAST");
  Serial.printf("] %s\n", status == 0 ? "OK" : "FAIL");
}

// ---------- esp-now helpers ----------
static bool addPeerCh(const uint8_t *mac, uint8_t ch) {
  if (esp_now_is_peer_exist((uint8_t *)mac)) esp_now_del_peer((uint8_t *)mac);
  int r = esp_now_add_peer((uint8_t *)mac, ESP_NOW_ROLE_COMBO, ch, NULL, 0);
  return r == 0;
}
static inline void sendBcast(const char *s, int len) {
  esp_now_send((uint8_t *)BCAST_MAC, (uint8_t *)s, len);
}

// ---------- parse JOB ----------
static bool parseJobLine(const char *line) {
  // JOB,<lastHash(40)>,<expectedHex(40)>,<diff>[,<jobTag(8)>]
  char lastHash[41] = { 0 }, expect[41] = { 0 }, tag[9] = { 0 };

  unsigned diff = 0;
  int n = sscanf(line, "JOB,%40[^,],%40[^,],%u,%8s", lastHash, expect, &diff, tag);

  if (n < 3) return false;
  job_last_hash = String(lastHash);
  hexToBytes40(expect, job_expected);

  // diff→target
  job_targets = diff * 100 + 1;

  // pre-seed DSHA1
  dsha1.reset().write((const unsigned char *)job_last_hash.c_str(), job_last_hash.length());

  // jobTag: pakai dari gateway kalau ada; kalau tidak, ambil 8 tail dari lastHash
  if (n == 4 && strlen(tag) == 8) {
    strncpy(g_jobTag, tag, 8);
    g_jobTag[8] = '\0';
  } else {
    size_t L = job_last_hash.length();
    for (int i = 0; i < 8; i++) g_jobTag[i] = job_last_hash.c_str()[L - 8 + i];
    g_jobTag[8] = '\0';
  }
  return true;
}

// ---------- LED ----------
static inline void ledOn() {
  digitalWrite(LED_BUILTIN, LOW);
}
static inline void ledOff() {
  digitalWrite(LED_BUILTIN, HIGH);
}
static void blinkAck(bool good) {
  int count = good ? 2 : 1;
  for (int i = 0; i < count; i++) {
    ledOn();
    delay(80);
    ledOff();
    delay(90);
  }
}

void setup() {
  Serial.begin(115200);
  delay(150);
  Serial.println();
  Serial.println(F("[WKR] boot"));

  NODE_ID = chipHexUpper();
  CHIP_STR = chipHexLower();
  RIG_NAME = (String(RIG_IDENTIFIER) == "None") ? (String("ESP01S-") + NODE_ID)
                                                : (String(RIG_IDENTIFIER) + "-" + NODE_ID);

  pinMode(LED_BUILTIN, OUTPUT);
  ledOff();

  // radio/esp-now init
  wifi_set_opmode_current(STATION_MODE);
  wifi_set_phy_mode(PHY_MODE_11G);
  wifi_set_sleep_type(NONE_SLEEP_T);
  WiFi.persistent(false);
  WiFi.setAutoConnect(false);
  WiFi.setAutoReconnect(false);
  WiFi.disconnect(true);
  WiFi.mode(WIFI_STA);
  WiFi.setSleepMode(WIFI_NONE_SLEEP);

  // lock channel
  wifi_promiscuous_enable(1);
  wifi_set_channel(WIFI_CHANNEL);
  wifi_promiscuous_enable(0);
  Serial.printf("[WKR %s] ch=%u\n", NODE_ID.c_str(), WIFI_CHANNEL);

  int r = esp_now_init();
  Serial.printf("[ESPNOW] init=%d\n", r);
  if (r != 0) {
    Serial.println("[FATAL] esp_now_init");
    while (1) delay(1000);
  }

  esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
  esp_now_register_recv_cb(onRecv);
  esp_now_register_send_cb(onSent);

  // broadcast peer only (uplink broadcast)
  addPeerCh(BCAST_MAC, WIFI_CHANNEL);

  // salam awal
  String hello = String("HELLO_NODE,") + NODE_ID + "\n";
  sendBcast(hello.c_str(), hello.length());
}

void loop() {
  // pairing (belajar MAC STA gateway dari HELLO_GW)
  if (!paired) {
    static uint32_t t = 0;
    if (millis() - t >= 1000) {
      t = millis();
      String hello = String("HELLO_NODE,") + NODE_ID + "\n";
      sendBcast(hello.c_str(), hello.length());
      Serial.printf("[WKR %s] HELLO_NODE\n", NODE_ID.c_str());
    }
    if (got) {
      got = false;
      String line(rx);
      line.trim();
      if (line.startsWith("HELLO_GW")) {
        memcpy(DEST_MAC, lastSrc, 6);
        addPeerCh(DEST_MAC, WIFI_CHANNEL);  // agar bisa terima unicast dari GW
        // kirim ACK unicast singkat
        String ack = String("HELLO_ACK,") + NODE_ID + "\n";
        esp_now_send(DEST_MAC, (uint8_t *)ack.c_str(), ack.length());
        paired = true;
        Serial.print("[WKR] paired @ ");
        printMac(DEST_MAC);
        Serial.println();
      }
    }
    delay(1);
    return;
  }

  // REQJOB (uplink broadcast) + jitter 0-300ms
  delay(random(0, 300));
  {
    String req = String("REQJOB,") + NODE_ID + "\n";
    sendBcast(req.c_str(), req.length());
    // log ringkas
    Serial.printf("[WKR %s] REQJOB\n", NODE_ID.c_str());
  }

  // Tunggu JOB
  uint32_t t0 = millis();
  bool gotJob = false;
  while (millis() - t0 < 15000) {
    if (got) {
      got = false;
      String line(rx);
      line.trim();
      if (line.startsWith("JOB,")) {
        gotJob = parseJobLine(line.c_str());
        if (gotJob) {
          Serial.printf("[WKR %s] JOB diff=%u tag=%s\n", NODE_ID.c_str(), job_targets / 100, g_jobTag);
        }
        break;
      }
    }
    delay(1);
  }
  if (!gotJob) {
    Serial.printf("[WKR %s] no JOB\n", NODE_ID.c_str());
    delay(400);
    return;
  }

  // Mining (inner loop minimal)
  Counter<10> ctr;
  uint8_t hash[20];
  unsigned long tStart = micros();
  bool found = false;
  unsigned long foundNonce = 0;

  for (ctr.reset(); (unsigned int)ctr < job_targets; ++ctr) {
    DSHA1 ctx = dsha1;  // pre-seeded
    ctx.write((const unsigned char *)ctr.c_str(), ctr.strlen()).finalize(hash);
    if (memcmp(job_expected, hash, 20) == 0) {
      found = true;
      foundNonce = (unsigned int)ctr;
      break;
    }
    if (((unsigned int)ctr % 5000U) == 0U) delay(1);  // yield
  }
  // Di mineJob()
  unsigned long dt_us = micros() - tStart;
  float dt_s = dt_us * 1e-6f;
  if (dt_s < 1e-6f) dt_s = 1e-6f;  // Min 1us hindari div0/inflasi
  static float ema_hps = 55000.0;  // Seed awal
  const float alpha = 0.05;
  float hps = ((unsigned int)ctr) / dt_s;
  ema_hps = alpha * hps + (1 - alpha) * ema_hps;
  hps = ema_hps;

  // SUBMIT (uplink broadcast-only) + jobTag di akhir
  char out[220];
  snprintf(out, sizeof(out),
           "SUBMIT,%lu,0,%s,%s,%s,%s,%s\n",
           found ? foundNonce : (unsigned long)((unsigned int)ctr),
           RIG_NAME.c_str(), CHIP_STR.c_str(),
           "WALL@GW",  // placeholder, diabaikan GW
           NODE_ID.c_str(),
           g_jobTag);
  sendBcast(out, (int)strlen(out));
  Serial.printf("dt_us=%lu ctr=%u raw_hps=%.2f\n", dt_us, (unsigned int)ctr, hps);
  Serial.printf("[WKR %s] SUBMIT nonce=%lu kh/s=%.2f\n",
                NODE_ID.c_str(),
                found ? foundNonce : (unsigned long)((unsigned int)ctr),
                hps);

  // ACK
  const char ping = 'P';

  t0 = millis();
  bool gotAck = false;
  bool good = false;
  while (millis() - t0 < 15000) {
    if (got) {
      got = false;
      String line(rx);
      line.trim();
      if (line.startsWith("ACK,")) {
        // ACK,<resp>,<ping>
        int c1 = line.indexOf(','), c2 = line.indexOf(',', c1 + 1);
        String resp = line.substring(c1 + 1, c2 < 0 ? line.length() : c2);
        good = resp.startsWith("GOOD");
        Serial.printf("[WKR %s] ACK %s\n", NODE_ID.c_str(), resp.c_str());
        gotAck = true;
        break;
      }
    }
    delay(1);
  }
  blinkAck(good);
  if (!gotAck) Serial.printf("[WKR %s] no ACK\n", NODE_ID.c_str());

  delay(300);
}
