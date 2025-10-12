<h1>Duino-Coin ESP-NOW Gateway & Worker (ESP32 + ESP-01)</h1>

> Status: fresh project — works end-to-end in testing, but hasn’t been tried with a large number of devices yet. 
> Expect some rough edges and please report issues 🙏

<h3>What this is</h3>
A master–slave (gateway–worker) setup for Duino-Coin mining on ESP devices:
<ul>
  <li>ESP32 = Gateway <br>
      Connects to Wi-Fi + Duino pool, requests jobs, bridges data to workers via ESP-NOW, submits results back to the pool.
  </li>
  <li>ESP-01 / ESP8266 = Worker <br>
      Receives jobs via ESP-NOW, performs DSHA1 mining locally, sends results back via ESP-NOW.    
  </li>
</ul>  
Why? To mine with many ESP-01 while your router only counts one Wi-Fi client (the ESP32), because workers don’t join Wi-Fi — they talk to the gateway over ESP-NOW.
<hr>
<h3>Features</h3>
<ul>
  <li>ESP-NOW bridge: Worker ←→ Gateway (no AP join for workers).</li>
  <li>Auto pairing (no MAC lists): Broadcast handshake; workers learn the gateway’s actual STA MAC automatically.</li>
  <li>Uplink broadcast, downlink unicast: Robust against ACK quirks on ESP8266 → ESP32.</li>
  <li>Difficulty target: Gateway requests jobs with ESP8266 tag (typical diff ≈ 4000).</li>
  <li>Per-worker identifier: Each worker auto-names its rig: ESP01S-<NODE_ID> (e.g. ESP01S-D4F8A9).</li>
  <li>LED on worker: blink 2× on GOOD, 1× on BAD.</li>
  <li>DNS-patched pool connect: Discover /getPool via HTTP → connect by IP, not hostname (more resilient).</li>
</ul>
<hr>
<h3>How it works (in baby steps)</h3>
<ol>
  <li>
    <b>Gateway connects to your Wi-Fi and calls /getPool (HTTP).</b><br>
    It parses ip + port, then opens a TCP connection by IP.
  </li>
  <li>ESP-NOW handshake:<br>
    <ul>
      <li>Gateway broadcasts HELLO_GW.</li>
      <li>Workers broadcast HELLO_NODE,<NODE_ID> and unicast HELLO_ACK back after learning gateway’s STA MAC.</li>
    </ul>
  </li>
  <li>Worker asks for job:<br>
      REQJOB,<NODE_ID> (broadcast uplink).</li>
  <li>Gateway → Pool → Worker:<br>
  Gateway sends JOB,<user>,ESP8266,<key> → gets one job → sends to worker as
JOB,<lastHash>,<expectedHex>,<diff> (unicast).</li>
  <li>Worker mines (local DSHA1):<br>
      Tries nonces up to diff*100+1. Computes hash; if equals expected → found.</li>
  <li>Worker submits (uplink broadcast):<br>
  SUBMIT,<nonce>,<khps>,<rig>,<chip>,<user>,<node_id>
  </li>
  <li>Gateway → Pool:<br>
  Forwards submit in Duino’s canonical format; pool returns GOOD/BAD/....
Gateway replies to worker: ACK,<resp>,<ping_ms> (unicast).
Worker LED blinks: 2× for GOOD, 1× for BAD. Then repeats from step 3.</li>
</ol>
<hr>
Multi-ESP notes

Workers use random jitter on REQJOB to reduce collisions.

Gateway maintains a small registry {MAC, NODE_ID} discovered via handshake.

Uplink (worker→gateway) uses broadcast for reliability; downlink is unicast.

For 10–20 workers, this method is practical. For larger fleets, see Roadmap.

<hr>

<h3>Expected serial logs (happy path)</h3>

Gateway (ESP32):

[GW] boot
[GW] WiFi... OK IP=192.168.x.y ch=1 STA=44:1D:...
[PEER D4F8A9] HELLO_NODE
[JOB D4F8A9] <lastHash,expected,diff>
[SUBMIT D4F8A9] GOOD (154ms)

Worker (ESP-01):

[WKR] boot
[WKR D4F8A9] HELLO_NODE
[WKR D4F8A9] REQJOB
[WKR D4F8A9] JOB diff=4000
[WKR D4F8A9] SUBMIT nonce=... kh/s=...
[WKR D4F8A9] ACK GOOD

<hr>

Contributing

PRs welcome!
Please include logs for both gateway & worker when reporting bugs (first 20–30 lines that show the issue).

<hr>

License
MIT (proposed). If you need a different license, open an issue.

<hr>

<h3>Disclamer</h3>
This is a new project. It has not been tested at mass scale. Use at your own risk; keep an eye on device temps and power rails. If something feels flaky, file an issue and we’ll fix it. Happy hacking! 💡🔧

