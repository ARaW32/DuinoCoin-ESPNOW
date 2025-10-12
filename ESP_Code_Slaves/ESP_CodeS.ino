// ===== ESP01_Worker_ESPNow_Slave_FINAL.ino =====
// Board: ESP8266 (ESP-01)
// Baud: 115200

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <espnow.h>
#include <user_interface.h>

#include "Counter.h"
#include "DSHA1.h"
#include "Settings.h"   // DUCO_USER, MINER_KEY, RIG_IDENTIFIER (warning extern abaikan)

// ===== USER CONFIG =====
static const uint8_t WIFI_CHANNEL = 1;      // samakan dgn channel AP/gateway
// (opsional) hint MAC gateway; worker tetap pakai MAC sumber HELLO_GW
uint8_t GATEWAY_MAC_HINT[6] = { 0xXX,0xXX,0xXX,0xXX,0xXX,0xXX };
// uplink mode: broadcast-only untuk stabil (hindari onSent FAIL unicast)
#define USE_UNICAST_UPLINK 0

#ifndef LED_BUILTIN
#define LED_BUILTIN 2
#endif

// =======================

static const uint8_t BCAST_MAC[6]={0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

volatile bool got=false;
char     rx[240];
uint8_t  lastSrc[6];
uint8_t  DEST_MAC[6]={0};    // MAC STA gateway untuk unicast balasan
bool     paired=false;

String NODE_ID;               // ex: "D4F8A9"
String RIG_NAME;              // ex: "ESP01S-D4F8A9"
String CHIP_STR;              // ex: "d4f8a9" (lowercase)

DSHA1 dsha1;
uint8_t job_expected[20];
String  job_last_hash;
unsigned int job_targets=0;

// ---------- utils ----------
static inline void printMac(const uint8_t *m){
  Serial.printf("%02X:%02X:%02X:%02X:%02X:%02X",m[0],m[1],m[2],m[3],m[4],m[5]);
}
static inline uint8_t hexNibble(char c){
  if(c>='0'&&c<='9') return c-'0';
  if(c>='A'&&c<='F') return c-'A'+10;
  if(c>='a'&&c<='f') return c-'a'+10;
  return 0;
}
static void hexToBytes40(const String &s, uint8_t out20[20]){
  for(int i=0;i<20;i++){
    uint8_t hi=hexNibble(s[2*i]), lo=hexNibble(s[2*i+1]); out20[i]=(hi<<4)|lo;
  }
}
static String chipHexUpper(){
  uint8_t mac[6]; wifi_get_macaddr(STATION_IF, mac);
  char nid[7]; sprintf(nid,"%02X%02X%02X", mac[3],mac[4],mac[5]);
  return String(nid);
}
static String chipHexLower(){
  uint8_t mac[6]; wifi_get_macaddr(STATION_IF, mac);
  char nid[7]; sprintf(nid,"%02x%02x%02x", mac[3],mac[4],mac[5]);
  return String(nid);
}

// ---------- callbacks ----------
void onRecv(uint8_t *mac, uint8_t *data, uint8_t len){
  int n=min((int)len,(int)sizeof(rx)-1);
  memcpy(lastSrc, mac, 6);
  memcpy(rx, data, n); rx[n]='\0';
  got=true;
}
void onSent(uint8_t *mac, uint8_t status){
  // Jangan spam log untuk broadcast
  if (mac && memcmp(mac, BCAST_MAC, 6)==0) return;
  Serial.print("[TX:sent ");
  if (mac) printMac(mac); else Serial.print("BCAST");
  Serial.printf("] %s\n", status==0?"OK":"FAIL");
}

// ---------- espnow helpers ----------
static bool addPeerCh(const uint8_t *mac, uint8_t ch){
  if(esp_now_is_peer_exist((uint8_t*)mac)) esp_now_del_peer((uint8_t*)mac);
  int r=esp_now_add_peer((uint8_t*)mac, ESP_NOW_ROLE_COMBO, ch, NULL, 0);
  Serial.print("[ESPNOW] add_peer "); printMac(mac); Serial.printf(" ch=%u -> %d\n", ch, r);
  return r==0;
}
static bool addBroadcastPeer(uint8_t ch){ return addPeerCh(BCAST_MAC, ch); }

// uplink helper (broadcast utama; unicast opsional)
static void uplink(const String &s){
  esp_now_send((uint8_t*)BCAST_MAC, (uint8_t*)s.c_str(), (int)s.length());
  #if USE_UNICAST_UPLINK
    if (DEST_MAC[0]|DEST_MAC[1]|DEST_MAC[2]|DEST_MAC[3]|DEST_MAC[4]|DEST_MAC[5])
      esp_now_send((uint8_t*)DEST_MAC, (uint8_t*)s.c_str(), (int)s.length());
  #endif
}

// ---------- job parsing ----------
static bool parseJob(const String &line){
  // JOB,<lastHash>,<expectHex>,<diff>
  int p1=line.indexOf(','), p2=line.indexOf(',',p1+1), p3=line.indexOf(',',p2+1);
  if(p1<0||p2<0||p3<0) return false;
  job_last_hash = line.substring(p1+1,p2);
  String expect = line.substring(p2+1,p3);
  int diff      = line.substring(p3+1).toInt();
  if(expect.length()!=40) return false;
  hexToBytes40(expect, job_expected);
  job_targets = diff*100 + 1;
  dsha1.reset().write((const unsigned char*)job_last_hash.c_str(), job_last_hash.length());
  return true;
}

// ---------- LED ----------
static inline void ledOn(){ digitalWrite(LED_BUILTIN, LOW); }
static inline void ledOff(){ digitalWrite(LED_BUILTIN, HIGH); }
static void blinkAck(bool good){
  // GOOD: 2 blink; BAD: 1 blink
  int count = good ? 2 : 1;
  for(int i=0;i<count;i++){
    ledOn();  delay(80);
    ledOff(); delay(90);
  }
}

void setup(){
  Serial.begin(115200); delay(150);
  Serial.println();
  Serial.println(F("[WKR] boot"));

  // Node identifiers
  NODE_ID  = chipHexUpper();
  CHIP_STR = chipHexLower();
  RIG_NAME = (String(RIG_IDENTIFIER)=="None") ? (String("ESP01S-")+NODE_ID)
                                              : (String(RIG_IDENTIFIER)+"-"+NODE_ID);

  pinMode(LED_BUILTIN, OUTPUT);
  ledOff();

  // radio untuk esp-now
  wifi_set_opmode_current(STATION_MODE);
  wifi_set_phy_mode(PHY_MODE_11G);
  wifi_set_sleep_type(NONE_SLEEP_T);
  WiFi.persistent(false); WiFi.setAutoConnect(false); WiFi.setAutoReconnect(false);
  WiFi.disconnect(true); WiFi.mode(WIFI_STA); WiFi.setSleepMode(WIFI_NONE_SLEEP);
  // lock channel
  wifi_promiscuous_enable(1); wifi_set_channel(WIFI_CHANNEL); wifi_promiscuous_enable(0);
  Serial.printf("[WKR] ch=%u, ID=%s\n", WIFI_CHANNEL, NODE_ID.c_str());

  int r=esp_now_init();
  Serial.printf("[ESPNOW] init=%d\n", r);
  if(r!=0){ Serial.println("[FATAL] esp_now_init"); while(1) delay(1000); }

  esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
  esp_now_register_recv_cb(onRecv);
  esp_now_register_send_cb(onSent);

  addBroadcastPeer(WIFI_CHANNEL);
  addPeerCh(GATEWAY_MAC_HINT, WIFI_CHANNEL); // opsional

  // random jitter anti tabrakan
  randomSeed(system_get_time());

  // salam awal
  String hello = String("HELLO_NODE,")+NODE_ID+"\n";
  uplink(hello);
}

void loop(){
  // pairing
  if(!paired){
    static uint32_t t=0;
    if(millis()-t >= 1000){
      t=millis();
      String hello = String("HELLO_NODE,")+NODE_ID+"\n";
      uplink(hello);
      Serial.printf("[WKR %s] HELLO_NODE\n", NODE_ID.c_str());
    }
    if(got){
      got=false; String line(rx); line.trim();
      if(line.startsWith("HELLO_GW")){
        Serial.print("[WKR] HELLO_GW from "); printMac(lastSrc); Serial.println();
        memcpy(DEST_MAC,lastSrc,6);
        addPeerCh(DEST_MAC, WIFI_CHANNEL);
        String ack = String("HELLO_ACK,")+NODE_ID+"\n";
        esp_now_send(DEST_MAC, (uint8_t*)ack.c_str(), ack.length());
        paired=true;
        Serial.print("[WKR] paired @ "); printMac(DEST_MAC); Serial.println();
      }
    }
    delay(1);
    return;
  }

  // 3) REQJOB (dengan jitter 0-300ms)
  delay(random(0, 300));
  {
    String req = String("REQJOB,")+NODE_ID+"\n";
    uplink(req);
    Serial.printf("[WKR %s] REQJOB\n", NODE_ID.c_str());
  }

  // 4) tunggu JOB
  uint32_t t0=millis(); bool gotJob=false;
  while(millis()-t0<15000){
    if(got){
      got=false; String line(rx); line.trim();
      if(line.startsWith("JOB,")){
        gotJob = parseJob(line);
        Serial.printf("[WKR %s] JOB diff=%u\n", NODE_ID.c_str(), job_targets/100);
        break;
      }
    }
    delay(1);
  }
  if(!gotJob){ Serial.printf("[WKR %s] no JOB\n", NODE_ID.c_str()); delay(500); return; }

  // mining
  Counter<10> ctr;
  uint8_t hash[20];
  unsigned long tStart=micros();
  bool found=false; unsigned long foundNonce=0;

  for(ctr.reset(); (unsigned int)ctr < job_targets; ++ctr){
    DSHA1 ctx = dsha1; // pre-seeded
    ctx.write((const unsigned char*)ctr.c_str(), ctr.strlen()).finalize(hash);
    if (memcmp(job_expected, hash, 20)==0){ found=true; foundNonce=(unsigned int)ctr; break; }
    if (((unsigned int)ctr % 5000U)==0U) delay(1);
  }
  unsigned long dt_us=micros()-tStart;
  float dt_s=max(0.001f, dt_us*1e-6f);
  float hps=((unsigned int)ctr)/dt_s;

  // 5) SUBMIT (uplink broadcast for reliability)
  String wallet = String(DUCO_USER);
  String submit = String("SUBMIT,") + String(found?foundNonce:(unsigned long)((unsigned int)ctr)) + "," +
                  String(hps,2) + "," + RIG_NAME + "," + CHIP_STR + "," + wallet + "," + NODE_ID + "\n";
  uplink(submit);
  Serial.printf("[WKR %s] SUBMIT nonce=%lu kh/s=%.2f\n", NODE_ID.c_str(),
                found?(unsigned long)foundNonce:(unsigned long)((unsigned int)ctr), hps);

  // 6) ACK
  t0=millis(); bool gotAck=false; bool good=false;
  while(millis()-t0<12000){
    if(got){
      got=false; String line(rx); line.trim();
      if(line.startsWith("ACK,")){
        // ACK,<resp>,<ping>
        int c1=line.indexOf(','), c2=line.indexOf(',', c1+1);
        String resp=line.substring(c1+1, c2<0?line.length():c2);
        good = resp.startsWith("GOOD");
        Serial.printf("[WKR %s] ACK %s\n", NODE_ID.c_str(), resp.c_str());
        gotAck=true; break;
      }
    }
    delay(1);
  }
  blinkAck(good);
  if(!gotAck) Serial.printf("[WKR %s] no ACK\n", NODE_ID.c_str());

  delay(350);
}
