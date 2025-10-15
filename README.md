<h1 align="center">⚙️ Duino-Coin ESP-NOW Gateway & Worker (ESP32 + ESP-01)</h1>

<p align="center">
  <b>ESP-NOW powered distributed Duino-Coin miner</b><br>
  <i>Mine with many ESP-01s — while your router only sees one device!</i>
</p>

<p align="center">
  <img src="https://img.shields.io/badge/status-fresh--project-blue?style=flat-square">
  <img src="https://img.shields.io/badge/license-MIT-green?style=flat-square">
  <img src="https://img.shields.io/badge/platform-ESP32%20%2B%20ESP8266-orange?style=flat-square">
</p>

---

> 🧪 **Status:** Fresh project — works end-to-end in testing, but not yet stress-tested with large device counts.  
> ⚠️ **Last issue:** `BAD Submit`, `Modified Hashrate` — Solved.  
> ⚠️ **Current issue:** `High Starting Difficulty` when mining, resulting to `Modified Hashrate` problem — Being Investigated.   
> 🧰 Expect rough edges — PRs & bug reports are *very welcome!*

---

## 💡 What This Is

A **master–worker** (gateway–worker) setup for [Duino-Coin](https://github.com/revoxhere/duino-coin) mining using ESP devices.

| Role | Device | Description |
|------|---------|-------------|
| 🖧 **Gateway** | ESP32 | Connects to Wi-Fi and Duino pool, fetches jobs, bridges them to workers via ESP-NOW, and submits results back. |
| ⚙️ **Worker** | ESP-01 / ESP8266 | Receives mining jobs over ESP-NOW, computes DSHA1 locally, and sends results back to the gateway. |

**Why this project?**  
Because you can mine with *many* ESP-01s while only **one** device connects to your router.  
The workers never join Wi-Fi — they use ESP-NOW for lightweight, low-power communication.

---

## ✨ Features

- 🔁 **ESP-NOW bridge:** Worker ↔ Gateway (no Wi-Fi join needed for workers).  
- 🧭 **Auto pairing:** Broadcast handshake — no manual MAC setup.  
- 📡 **Smart communication:** Uplink = broadcast, Downlink = unicast (solves ACK quirks).  
- 🎯 **Adaptive difficulty:** Requests jobs tagged as `ESP8266` (≈ diff 4000).  
- 🆔 **Auto rig naming:** `ESP01S-<NODE_ID>` (e.g. `ESP01S-D4F8A9`).  
- 💡 **Status LED:** Blink `2× GOOD`, `1× BAD`.  
- 🌐 **DNS-patched pool connect:** Resolves `/getPool` via HTTP → connects by IP (faster, more resilient).

---

## 🧩 How It Works — Step by Step

1. **Gateway boot-up**
   - Connects to Wi-Fi and calls `/getPool` (HTTP).  
   - Parses IP + port, then opens TCP connection.

2. **ESP-NOW Handshake**
   - Gateway broadcasts `HELLO_GW`.  
   - Worker broadcasts `HELLO_NODE,<NODE_ID>` and unicasts `HELLO_ACK` after learning gateway’s STA MAC.

3. **Worker requests job**
   - Sends `REQJOB,<NODE_ID>` (broadcast).

4. **Gateway → Pool → Worker**
   - Gateway requests one job (`JOB,<user>,ESP8266,<key>`) from pool.  
   - Sends to worker: `JOB,<lastHash>,<expectedHex>,<diff>` (unicast).

5. **Worker mines locally**
   - Performs DSHA1 up to `diff*100+1` nonces.  
   - Compares hash with expected.

6. **Worker submits result**
   - Broadcasts: `SUBMIT,<nonce>,<khps>,<rig>,<chip>,<user>,<node_id>`

7. **Gateway → Pool → Worker**
   - Forwards submit in canonical format.  
   - Pool replies `GOOD` / `BAD` / etc.  
   - Gateway responds: `ACK,<resp>,<ping_ms>` (unicast).  
   - Worker LED blinks accordingly, then loops back to step 3.

---

## 🧠 Notes for Multi-ESP Setups

- Workers add **random jitter** on `REQJOB` to prevent broadcast collisions.  
- Gateway maintains a small registry `{MAC, NODE_ID}` for active workers.  
- Uplink uses **broadcast** for reliability; downlink uses **unicast** for precision.  
- Works best for **10–20 workers**. Beyond that, see roadmap for scaling ideas.

---

## 🤝 Contributing

Pull requests welcome!  
If you encounter a bug, please include:
- Logs from both **gateway & worker**
- First **20–30 lines** showing the handshake and job flow

---

## ⚖️ License

MIT (proposed).  
Need a different license? → Open an issue.

---

## ⚠️ Disclaimer

This is a **new project** — not tested at large scale.  
Use at your own risk.  
Keep an eye on:
- 🔥 Device temperatures  
- ⚡ Power rail stability  
 
Happy mining! 💡🔧
