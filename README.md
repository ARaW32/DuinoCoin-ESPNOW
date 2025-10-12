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
  <li></li>
  <li></li>
  <li></li>
  <li></li>
  <li></li>
  <li></li>
</ol>



ESP-NOW handshake:

Gateway broadcasts HELLO_GW.

Workers broadcast HELLO_NODE,<NODE_ID> and unicast HELLO_ACK back after learning gateway’s STA MAC.

Worker asks for job:
REQJOB,<NODE_ID> (broadcast uplink).

Gateway → Pool → Worker:
Gateway sends JOB,<user>,ESP8266,<key> → gets one job → sends to worker as
JOB,<lastHash>,<expectedHex>,<diff> (unicast).

Worker mines (local DSHA1):
Tries nonces up to diff*100+1. Computes hash; if equals expected → found.

Worker submits (uplink broadcast):
SUBMIT,<nonce>,<khps>,<rig>,<chip>,<user>,<node_id>

Gateway → Pool:
Forwards submit in Duino’s canonical format; pool returns GOOD/BAD/....
Gateway replies to worker: ACK,<resp>,<ping_ms> (unicast).
Worker LED blinks: 2× for GOOD, 1× for BAD. Then repeats from step 3.
