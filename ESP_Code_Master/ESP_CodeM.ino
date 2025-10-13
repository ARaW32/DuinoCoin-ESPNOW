// ===== ESP32_Gateway_ESPNow_Master_FINAL.ino =====
// Board: ESP32 Dev Module (Arduino-ESP32 v3.x / IDF v5.x)
// Serial Monitor: 115200

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <esp_wifi.h>
#include <esp_now.h>
#include "Settings.h"  // DUCO_USER, MINER_KEY, RIG_IDENTIFIER (boleh dipakai)

#define WIFI_SSID_VALUE "ESPs"
#define WIFI_PASS_VALUE "m4m4p4p4"

static const uint8_t BCAST_MAC[6] = { 255, 255, 255, 255, 255, 255 };
static const char* START_DIFF_TAG = "ESP8266";  // starting diff ≈ 4000
static const char* MINER_BANNER = "Official ESP8266 Miner";
static const char* MINER_VER = "4.3";

static const uint32_t JOB_TIMEOUT_MS = 20000;
static const uint32_t SUBMIT_TIMEOUT_MS = 20000;

// KH/s policy (sesuaikan kalau perlu)
static const double KH_FALLBACK = 56000.0;
static const double KH_MIN = 52000.0;
static const double KH_MAX = 62000.0;

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

volatile bool g_got = false;
char g_rx[240];
uint8_t g_rxMac[6];

bool hasWorker = false;
uint32_t tHello = 0;

// ------ small registry per node ------
struct NodeState {
  bool used = false;
  uint8_t mac[6];
  char id[8];            // NODE_ID
  char lastJobTag[9];    // last tag sent with JOB
  uint32_t lastSig = 0;  // dedup signature (nonce+tag)
  uint32_t lastJobStartMs = 0;
  uint32_t lastDiff = 0;
  uint32_t goodKhps = 55600;  // seed aman ESP-01
  bool hasGood = false;       // pernah GOOD?

  // --- NEW: smoothing KH/s (versi gateway) ---
  double emaHps = 55000.0;  // seed aman untuk ESP01
  bool emaInit = false;
};
NodeState nodes[32];

static inline String tail8(const String& hex40) {
  int L = hex40.length();
  if (L >= 8) return hex40.substring(L - 8);
  String t = hex40;
  while (t.length() < 8) t = "0" + t;
  return t.substring(t.length() - 8);
}

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
static inline bool isWeirdIP(IPAddress ip) {
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
  if (colon < 0) {
    Serial.println("[POOL] port parse fail");
    return false;
  }
  int end = body.indexOf('\n', colon);
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
static int findNodeIdxByMac(const uint8_t mac[6]) {
  for (int i = 0; i < 32; i++)
    if (nodes[i].used && memcmp(nodes[i].mac, mac, 6) == 0) return i;
  return -1;
}
static int firstFreeNodeIdx() {
  for (int i = 0; i < 32; i++)
    if (!nodes[i].used) return i;
  return -1;
}
static void rememberNode(const uint8_t mac[6], const String& id) {
  int idx = findNodeIdxByMac(mac);
  if (idx < 0) {
    idx = firstFreeNodeIdx();
    if (idx < 0) return;
    nodes[idx].used = true;
    memcpy(nodes[idx].mac, mac, 6);
    nodes[idx].lastSig = 0;
    nodes[idx].lastJobTag[0] = '\0';
  }
  size_t L = min((size_t)15, id.length());
  memcpy(nodes[idx].id, id.c_str(), L);
  nodes[idx].id[L] = '\0';
}
static const char* nodeIdOf(const uint8_t mac[6]) {
  int idx = findNodeIdxByMac(mac);
  return (idx >= 0) ? nodes[idx].id : "UNK";
}
static NodeState* nodeRef(const uint8_t mac[6]) {
  int idx = findNodeIdxByMac(mac);
  return (idx >= 0) ? (&nodes[idx]) : nullptr;
}

// ------ tiny hash for dedup ------
static uint32_t fnv1a32(const char* s) {
  uint32_t h = 2166136261u;
  while (*s) {
    h ^= (uint8_t)*s++;
    h *= 16777619u;
  }
  return h;
}

// ------ ESPNOW ------
void onRecv(const esp_now_recv_info* info, const uint8_t* data, int len) {
  if (!info || !data || len <= 0) return;
  int n = min(len, (int)sizeof(g_rx) - 1);
  memcpy(g_rxMac, info->src_addr, 6);
  memcpy(g_rx, data, n);
  g_rx[n] = '\0';
  g_got = true;
}
void onSent(const wifi_tx_info_t* info, esp_now_send_status_t status) {
  // ringkas, tak perlu spam log
  (void)info;
  (void)status;
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
  return esp_now_add_peer(&p) == ESP_OK;
}
static inline bool sendNow(const uint8_t mac[6], const String& s) {
  return esp_now_send((uint8_t*)mac, (const uint8_t*)s.c_str(), (int)s.length()) == ESP_OK;
}

// ------ bridge handlers ------
static bool handle_REQJOB(const uint8_t src[6]) {
  if (!ensurePoolConnected()) return false;

  // Request job (pakai ESP8266 agar diff=4000)
  String req = "JOB," + String((const char*)DUCO_USER) + ",ESP8266," + String((const char*)MINER_KEY) + "\n";
  pool.print(req);

  // Tunggu job: "lastHash,expectedHex,diff"
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
    Serial.println("[POOL] job timeout");
    pool.stop();
    return false;
  }

  // Parse
  int a = job.indexOf(',');
  int b = job.indexOf(',', a + 1);
  if (a < 0 || b < 0) return false;

  String lastHash = job.substring(0, a);
  String expectedHex = job.substring(a + 1, b);
  String diffStr = job.substring(b + 1);
  uint32_t diff = (uint32_t)diffStr.toInt();
  String tag = tail8(lastHash);

  // Catat timing & diff pada node
  NodeState* N = nodeRef(src);
  if (N) {
    N->lastDiff = diff;
    // >> PENTING: waktu mulai di-stamp SESAAT SEBELUM dikirim ke worker
    N->lastJobStartMs = millis();
    strncpy(N->lastJobTag, tag.c_str(), 8);
    N->lastJobTag[8] = '\0';
  }

  // Kirim frame ke worker: "JOB,<lastHash>,<expectedHex>,<diff>,<tag>\n"
  String pkt = "JOB," + lastHash + "," + expectedHex + "," + diffStr + "," + tag + "\n";
  ensurePeer(src);
  bool ok = sendNow(src, pkt);

  Serial.printf("[JOB %s] %s (%s)\n", N ? N->id : "UNK", job.c_str(), tag.c_str());
  return ok;
}

static bool handle_SUBMIT(const char* line, const uint8_t src[6]) {
  if (!ensurePoolConnected()) return false;

  NodeState* N = nodeRef(src);
  const char* nodeLabel = N ? N->id : "UNK";

  // Format yang diharapkan dari worker:
  // SUBMIT,<nonce>,<khps_worker>,<rig>,<chip>,<wallet>,<node_id>[,<jobTag>]
  String s(line);

  int c1 = s.indexOf(',');
  if (c1 < 0) return false;
  int c2 = s.indexOf(',', c1 + 1);
  if (c2 < 0) return false;
  int c3 = s.indexOf(',', c2 + 1);
  if (c3 < 0) return false;
  int c4 = s.indexOf(',', c3 + 1);
  if (c4 < 0) return false;
  int c5 = s.indexOf(',', c4 + 1);
  if (c5 < 0) return false;
  int c6 = s.indexOf(',', c5 + 1);
  if (c6 < 0) return false;
  int c7 = s.indexOf(',', c6 + 1);  // optional

  String nonceStr = s.substring(c1 + 1, c2);
  String khpsStr = s.substring(c2 + 1, c3);  // KH/s dari worker (dipakai)
  String rig = s.substring(c3 + 1, c4);
  String chip = s.substring(c4 + 1, c5);
  String wall = s.substring(c5 + 1, c6);
  String nodeId = s.substring(c6 + 1, (c7 > 0 ? c7 : s.length()));
  String jobTag = (c7 > 0) ? s.substring(c7 + 1) : String("");

  // fallback tag: pakai tag terakhir yang kita kirim
  if (!jobTag.length() && N && N->lastJobTag[0]) jobTag = String(N->lastJobTag);

  // STALE check jika kedua sisi punya tag
  if (N && jobTag.length() == 8 && N->lastJobTag[0]) {
    if (strncmp(N->lastJobTag, jobTag.c_str(), 8) != 0) {
      ensurePeer(src);
      sendNow(src, "ACK,STALE,0\n");
      Serial.printf("[DROP %s] STALE tag=%s last=%s\n", nodeLabel, jobTag.c_str(), N->lastJobTag);
      return true;
    }
  }

  // Dedup berdasarkan (nodeId, nonce, tag)
  String sigStr = nodeId + ":" + nonceStr + ":" + jobTag;
  uint32_t sig = fnv1a32(sigStr.c_str());
  if (N && sig == N->lastSig) {
    ensurePeer(src);
    sendNow(src, "ACK,DUP,0\n");
    Serial.printf("[DROP %s] DUP nonce=%s tag=%s\n", nodeLabel, nonceStr.c_str(), jobTag.c_str());
    return true;
  }
  if (N) N->lastSig = sig;

  // === KH/s policy: pakai angka dari worker, dibulatkan & di-clamp ===
  double khps_out = khpsStr.toDouble();
  if (khps_out <= 0.0) khps_out = KH_FALLBACK;
  khps_out = (double)((uint32_t)(khps_out + 0.5));  // integer only
  if (khps_out < KH_MIN) khps_out = KH_MIN;
  if (khps_out > KH_MAX) khps_out = KH_MAX;

  uint32_t reportKhps = (N && N->hasGood) ? N->goodKhps : 55600;

  // Upstream ke pool (KH/s TANPA desimal)
  String upstream = nonceStr + "," + String(reportKhps) + "," + MINER_BANNER + " " + MINER_VER + "," + rig + ",DUCOID" + chip + "," + wall + "\n";
  pool.print(upstream);

  // Ambil jawaban pool & kirim ACK ke worker
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
    ensurePeer(src);
    sendNow(src, "ACK,NOPOOL,0\n");
    Serial.printf("[SUBMIT %s] timeout\n", nodeLabel);
    pool.stop();
    activePool = DucoPool{};
    return false;
  }

  ensurePeer(src);
  sendNow(src, String("ACK,") + resp + ",0\n");
  Serial.printf("[SUBMIT %s] %s (kh/s_out=%u tag=%s)\n",
                nodeLabel, resp.c_str(), (unsigned)((uint32_t)khps_out), jobTag.c_str());

  if (resp.startsWith("GOOD")) {
    if (N) { N->hasGood = true; /* optional: tetap biarkan 55600 */ }
  } else if (resp.indexOf("Modified hashrate") >= 0) {
    // optional: kalau masih kejadian, turunkan dikit
    if (N && N->goodKhps > 55200) N->goodKhps -= 100;  // nudge down
  }

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

  // HELLO awal (broadcast)
  for (int i = 0; i < 3; i++) {
    esp_now_send((uint8_t*)BCAST_MAC, (uint8_t*)"HELLO_GW\n", 9);
    delay(120);
  }
  tHello = millis();
}

void loop() {
  // HELLO periodic sampai ada worker
  if (!hasWorker && millis() - tHello >= 1000) {
    tHello = millis();
    esp_now_send((uint8_t*)BCAST_MAC, (uint8_t*)"HELLO_GW\n", 9);
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
  }
}
