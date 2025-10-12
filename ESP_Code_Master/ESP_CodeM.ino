// ===== ESP32_Gateway_ESPNow_Master_FINAL.ino =====
// Board: ESP32 DevKit (Arduino-ESP32 v3.x / IDF v5.x)
// Baud: 115200

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <esp_wifi.h>
#include <esp_now.h>

#include "Settings.h"  // DUCO_USER, MINER_KEY, RIG_IDENTIFIER (boleh dipakai)

#ifndef WIFI_SSID_VALUE
#define WIFI_SSID_VALUE "SSID"
#endif
#ifndef WIFI_PASS_VALUE
#define WIFI_PASS_VALUE "PASS"
#endif

static const uint8_t BCAST_MAC[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
uint8_t KNOWN_WORKER_MAC[6] = {0,0,0,0,0,0};
static const uint8_t KNOWN_HINTS[][6] = {
  { 0x5C, 0xCF, 0x7F, 0xD4, 0xF8, 0xA9 },  // Slave #1
  // ADD SLAVE MAC ADDRESS HERE
};


// START_DIFF tag → set "ESP8266" agar starting diff ~ 4000 (bukan "ESP8266H")
static const char* START_DIFF_TAG = "ESP8266";
static const char* MINER_BANNER = "Official ESP8266 Miner";
static const char* MINER_VER = "4.3";

static const uint32_t JOB_TIMEOUT_MS = 20000;
static const uint32_t SUBMIT_TIMEOUT_MS = 20000;

static const size_t NUM_HINTS = sizeof(KNOWN_HINTS) / sizeof(KNOWN_HINTS[0]);

// ------------ pool state ------------
struct DucoPool {
  IPAddress ip;
  uint16_t port = 0;
  bool valid() const {
    return ip != IPAddress(0, 0, 0, 0) && port > 0;
  }
};
DucoPool activePool;
WiFiClient pool;

// ------------ now rx state ------------
volatile bool g_got = false;
char g_rx[240];
uint8_t g_rxMac[6];

bool hasWorker = false;
uint32_t tHello = 0;

// small registry
struct Node {
  uint8_t mac[6];
  char id[16];
  bool used = false;
};
Node nodes[32];

static inline void macToStr(const uint8_t* m, char* out) {
  sprintf(out, "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
}
static const char* errstr(esp_err_t e) {
  switch (e) {
    case ESP_OK: return "OK";
    case ESP_ERR_ESPNOW_NOT_INIT: return "NOT_INIT";
    case ESP_ERR_ESPNOW_ARG: return "ARG";
    case ESP_ERR_ESPNOW_INTERNAL: return "INTERNAL";
    case ESP_ERR_ESPNOW_NO_MEM: return "NO_MEM";
    case ESP_ERR_ESPNOW_NOT_FOUND: return "NOT_FOUND";
    case ESP_ERR_ESPNOW_IF: return "IF";
    case ESP_ERR_ESPNOW_EXIST: return "EXIST";
    default: return "?";
  }
}
static bool isWeirdIP(IPAddress ip) {
  if (ip == IPAddress(0, 0, 0, 0)) return true;
  if (ip[0] == 192 && ip[1] == 168 && ip[2] == 4) return true;
  return false;
}
static void ensureWiFiConnected() {
  if (WiFi.status() == WL_CONNECTED && !isWeirdIP(WiFi.localIP())) return;
  Serial.print("[GW] WiFi...");
  WiFi.disconnect(true, true);
  delay(200);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID_VALUE, WIFI_PASS_VALUE);
  uint32_t t0 = millis();
  while ((WiFi.status() != WL_CONNECTED || isWeirdIP(WiFi.localIP())) && millis() - t0 < 12000) {
    Serial.print('.');
    delay(250);
  }
  uint8_t mac_sta[6], mac_ap[6];
  esp_wifi_get_mac(WIFI_IF_STA, mac_sta);
  esp_wifi_get_mac(WIFI_IF_AP, mac_ap);
  char s_sta[20], s_ap[20];
  macToStr(mac_sta, s_sta);
  macToStr(mac_ap, s_ap);
  Serial.printf(" OK IP=%s ch=%d STA=%s AP=%s\n",
                WiFi.localIP().toString().c_str(), WiFi.channel(), s_sta, s_ap);
}

// DNS→IP (retry)
static bool resolveHostRetry(const char* host, IPAddress& out, int maxTry = 5) {
  out = IPAddress(0, 0, 0, 0);
  for (int i = 1; i <= maxTry; i++) {
    if (WiFi.hostByName(host, out) == 1 && out != IPAddress(0, 0, 0, 0)) {
      Serial.printf("[DNS] %s -> %s (try %d)\n", host, out.toString().c_str(), i);
      return true;
    }
    delay(200);
  }
  return false;
}
// GET /getPool via HTTP, ambil IP, lalu TCP by IP
static bool discoverPool(DucoPool& out) {
  ensureWiFiConnected();
  IPAddress pickerIP;
  if (!resolveHostRetry("server.duinocoin.com", pickerIP)) {
    Serial.println("[POOL] DNS picker fail");
    return false;
  }
  WiFiClient http;
  http.setTimeout(3000);
  Serial.printf("[POOL] picker %s:80 ... ", pickerIP.toString().c_str());
  if (!http.connect(pickerIP, 80)) {
    Serial.println("fail");
    return false;
  }
  Serial.println("ok");
  http.print(String("GET /getPool HTTP/1.1\r\n") + "Host: server.duinocoin.com\r\n" + "Connection: close\r\n\r\n");
  uint32_t t0 = millis();
  while (millis() - t0 < 3000) {
    String line = http.readStringUntil('\n');
    if (line.length() == 0 || line == "\r") break;
  }
  String body;
  t0 = millis();
  while (millis() - t0 < 3000) {
    while (http.available()) {
      body += http.readStringUntil('\n');
      body += '\n';
    }
    if (!http.connected()) break;
  }
  http.stop();
  if (!body.length()) {
    Serial.println("[POOL] empty body");
    return false;
  }
  int ipPos = body.indexOf("\"ip\":");
  int portPos = body.indexOf("\"port\":");
  if (ipPos < 0 || portPos < 0) {
    Serial.println("[POOL] invalid body");
    return false;
  }
  int q1 = body.indexOf('"', ipPos + 4), q2 = body.indexOf('"', q1 + 1);
  if (q1 < 0 || q2 < 0) {
    Serial.println("[POOL] ip parse fail");
    return false;
  }
  String ipStr = body.substring(q1 + 1, q2);
  int colon = body.indexOf(':', portPos);
  int end = body.indexOf('\n', colon);
  if (colon < 0) {
    Serial.println("[POOL] port parse fail");
    return false;
  }
  uint16_t prt = (uint16_t)body.substring(colon + 1, end > colon ? end : body.length()).toInt();
  IPAddress poolIP;
  if (!poolIP.fromString(ipStr)) {
    if (!resolveHostRetry(ipStr.c_str(), poolIP)) {
      Serial.println("[POOL] pool resolve fail");
      return false;
    }
  }
  out.ip = poolIP;
  out.port = prt;
  Serial.printf("[POOL] %s:%u\n", out.ip.toString().c_str(), out.port);
  return out.valid();
}
static bool ensurePoolConnected() {
  if (pool.connected()) return true;
  ensureWiFiConnected();
  if (!activePool.valid()) {
    Serial.println("[POOL] discover...");
    if (!discoverPool(activePool)) {
      Serial.println("[POOL] getPool FAIL");
      return false;
    }
  }
  Serial.printf("[POOL] TCP %s:%u ... ", activePool.ip.toString().c_str(), activePool.port);
  if (!pool.connect(activePool.ip, activePool.port)) {
    Serial.println("fail");
    return false;
  }
  pool.setTimeout(4000);
  String ver = pool.readStringUntil('\n');
  ver.trim();
  Serial.printf("ok (ver:%s)\n", ver.c_str());
  return true;
}

// ------ node registry ------
static void rememberNode(const uint8_t mac[6], const String& id) {
  int idx = -1;
  for (int i = 0; i < 32; i++) {
    if (nodes[i].used && memcmp(nodes[i].mac, mac, 6) == 0) {
      idx = i;
      break;
    }
    if (idx < 0 && !nodes[i].used) idx = i;
  }
  if (idx < 0) return;
  nodes[idx].used = true;
  memcpy(nodes[idx].mac, mac, 6);
  size_t L = min((size_t)15, id.length());
  memcpy(nodes[idx].id, id.c_str(), L);
  nodes[idx].id[L] = '\0';
}
static const char* nodeIdOf(const uint8_t mac[6]) {
  for (int i = 0; i < 32; i++)
    if (nodes[i].used && memcmp(nodes[i].mac, mac, 6) == 0) return nodes[i].id;
  return "UNK";
}

// ------ esp-now ------
void onRecv(const esp_now_recv_info* info, const uint8_t* data, int len) {
  if (!info || !data || len <= 0) return;
  int n = min(len, (int)sizeof(g_rx) - 1);
  memcpy(g_rxMac, info->src_addr, 6);
  memcpy(g_rx, data, n);
  g_rx[n] = '\0';
  g_got = true;
}
void onSent(const wifi_tx_info_t* info, esp_now_send_status_t status) {
  char m[20] = "??";
  if (info) macToStr(info->des_addr, m);
  // ringkas
  // Serial.printf("[TX %s] %s\n", m, status==ESP_NOW_SEND_SUCCESS?"OK":"FAIL");
}
static bool ensurePeer(const uint8_t* mac) {
  if (esp_now_is_peer_exist(mac)) return true;
  esp_now_peer_info_t p{};
  memcpy(p.peer_addr, mac, 6);
  p.ifidx = WIFI_IF_STA;
  p.channel = WiFi.channel();
  p.encrypt = false;
  esp_err_t r = esp_now_add_peer(&p);
  if (r != ESP_OK) {
    char mm[20];
    macToStr(mac, mm);
    Serial.printf("[ESPNOW] add_peer %s -> %s\n", mm, errstr(r));
  }
  return r == ESP_OK;
}
static bool ensureBroadcastPeer() {
  if (esp_now_is_peer_exist((uint8_t*)BCAST_MAC)) esp_now_del_peer((uint8_t*)BCAST_MAC);
  esp_now_peer_info_t p{};
  memcpy(p.peer_addr, BCAST_MAC, 6);
  p.ifidx = WIFI_IF_STA;
  p.channel = WiFi.channel();
  p.encrypt = false;
  esp_err_t r = esp_now_add_peer(&p);
  // Serial.printf("[ESPNOW] add_peer BROADCAST -> %s\n", (r==ESP_OK?"OK":"ERR"));
  return r == ESP_OK;
}
static bool sendNow(const uint8_t mac[6], const String& s) {
  return esp_now_send((uint8_t*)mac, (const uint8_t*)s.c_str(), (int)s.length()) == ESP_OK;
}

// ------ bridge handlers ------
static bool handle_REQJOB(const uint8_t src[6]) {
  const char* node = nodeIdOf(src);
  if (!ensurePoolConnected()) return false;
  // minta job pakai tag START_DIFF_TAG = "ESP8266" (diff ~ 4000)
  String req = String("JOB,") + DUCO_USER + "," + START_DIFF_TAG + "," + MINER_KEY + "\n";
  pool.print(req);
  uint32_t t0 = millis();
  String job;
  while (millis() - t0 < JOB_TIMEOUT_MS) {
    if (pool.available()) {
      job = pool.readStringUntil('\n');
      job.trim();
      break;
    }
    delay(1);
  }
  if (!job.length()) {
    Serial.printf("[JOB %s] timeout\n", node);
    pool.stop();
    activePool = DucoPool{};
    return false;
  }
  Serial.printf("[JOB %s] %s\n", node, job.c_str());
  ensurePeer(src);
  // kirim ke worker
  return sendNow(src, String("JOB,") + job + "\n");
}

static bool handle_SUBMIT(const char* line, const uint8_t src[6]) {
  const char* node = nodeIdOf(src);
  if (!ensurePoolConnected()) return false;
  // SUBMIT,<nonce>,<khps>,<rig>,<chip>,<wallet>[,<node_id>]
  String s(line);
  int c1 = s.indexOf(','), c2 = s.indexOf(',', c1 + 1), c3 = s.indexOf(',', c2 + 1),
      c4 = s.indexOf(',', c3 + 1), c5 = s.indexOf(',', c4 + 1), c6 = s.indexOf(',', c5 + 1);
  if (c1 < 0 || c2 < 0 || c3 < 0 || c4 < 0 || c5 < 0) return false;
  String nonce = s.substring(c1 + 1, c2);
  String khps = s.substring(c2 + 1, c3);
  String rig = s.substring(c3 + 1, c4);
  String chip = s.substring(c4 + 1, c5);
  String wall;
  String nodeId;
  if (c6 > 0) {
    wall = s.substring(c5 + 1, c6);
    nodeId = s.substring(c6 + 1);
  } else {
    wall = s.substring(c5 + 1);
    nodeId = String(node);
  }

  String rigTagged = rig;  // sudah ada -NODE_ID di worker; bisa dibiarkan

  String upstream = nonce + "," + khps + "," + MINER_BANNER + String(" ") + MINER_VER + "," + rigTagged + ",DUCOID" + chip + "," + wall + "\n";
  pool.print(upstream);

  uint32_t t0 = millis();
  String resp;
  while (millis() - t0 < SUBMIT_TIMEOUT_MS) {
    if (pool.available()) {
      resp = pool.readStringUntil('\n');
      resp.trim();
      break;
    }
    delay(1);
  }
  if (!resp.length()) {
    Serial.printf("[SUBMIT %s] timeout\n", node);
    pool.stop();
    activePool = DucoPool{};
    return false;
  }
  uint32_t ping_ms = millis() - t0;
  ensurePeer(src);
  sendNow(src, String("ACK,") + resp + "," + String(ping_ms) + "\n");
  Serial.printf("[SUBMIT %s] %s (%lums)\n", node, resp.c_str(), (unsigned long)ping_ms);
  return true;
}

void setup() {
  Serial.begin(115200);
  delay(150);
  Serial.println();
  Serial.println(F("[GW] boot"));

  ensureWiFiConnected();

  if (esp_now_init() != ESP_OK) {
    Serial.println("[ESPNOW] init FAIL");
    while (1) delay(1000);
  }
  esp_now_register_recv_cb(onRecv);
  esp_now_register_send_cb(onSent);

  ensureBroadcastPeer();
  ensurePeer(KNOWN_WORKER_MAC);

  // HELLO awal
  for (int i = 0; i < 3; i++) {
    esp_now_send((uint8_t*)BCAST_MAC, (uint8_t*)"HELLO_GW\n", 9);
    for (size_t i = 0; i < NUM_HINTS; i++) {
      ensurePeer(KNOWN_HINTS[i]);
      esp_now_send((uint8_t*)KNOWN_HINTS[i], (uint8_t*)"HELLO_GW\n", 9);
    }
    esp_now_send(KNOWN_WORKER_MAC, (uint8_t*)"HELLO_GW\n", 9);
    delay(120);
  }
  tHello = millis();
}

void loop() {
  // HELLO periodic hingga ada worker
  if (!hasWorker && millis() - tHello >= 1000) {
    tHello = millis();
    esp_now_send((uint8_t*)BCAST_MAC, (uint8_t*)"HELLO_GW\n", 9);
    esp_now_send(KNOWN_WORKER_MAC, (uint8_t*)"HELLO_GW\n", 9);
    Serial.println("[GW] HELLO");
  }

  if (!g_got) {
    delay(2);
    return;
  }
  noInterrupts();
  g_got = false;
  char buf[240];
  memcpy(buf, g_rx, sizeof(g_rx));
  uint8_t mac[6];
  memcpy(mac, g_rxMac, 6);
  interrupts();

  String line(buf);
  line.trim();
  const char* node = nodeIdOf(mac);

  if (line.startsWith("HELLO_NODE")) {
    String id = line.substring(line.indexOf(',') + 1);
    rememberNode(mac, id);
    hasWorker = true;
    ensurePeer(mac);
    esp_now_send(mac, (uint8_t*)"HELLO_GW\n", 9);
    Serial.printf("[PEER %s] HELLO_NODE\n", id.c_str());
  } else if (line.startsWith("HELLO_ACK")) {
    String id = line.substring(line.indexOf(',') + 1);
    rememberNode(mac, id);
    hasWorker = true;
    ensurePeer(mac);
    Serial.printf("[PEER %s] ACK\n", id.c_str());
  } else if (line.startsWith("REQJOB")) {
    handle_REQJOB(mac);
  } else if (line.startsWith("SUBMIT,")) {
    handle_SUBMIT(line.c_str(), mac);
  } else {
    // ignore
  }
}
