# Future plan: multiplayer / networking modules

**Status: PARKED — not to be started until the Halo 3 graphical issues in
Canary AE are resolved.** Recorded now so the design is not lost.

Covers two targets: the **original Xbox emulator** (the `x1-box` project — QEMU
based, see `project_x1box_new_emulator` in memory) and **Xenia-AE / Canary AE**
for Xbox 360.

Official servers are unavailable for both generations — Xbox Live 1.0 is offline
for the OG Xbox, and Xbox 360 emulators cannot obtain the required security
tokens. Both modules therefore target community replacements: **Insignia**
(OG Xbox) and **XLink Kai** / System Link tunnelling (both generations).

---

## 1. Original Xbox module (x1-box)

### What the services require

| Requirement | Detail |
|---|---|
| Unique identity | Insignia requires a valid, unique `eeprom.bin` holding the console's cryptographic keys and MAC address. **Two users connecting with the same EEPROM will be banned or fail to connect** — so the emulator must never ship or share a default EEPROM. |
| Proper initialisation | Needs the correct `mcpx_1.0.bin` boot ROM and a compatible flash ROM (BIOS), e.g. Complex 4627v1.03, to reach the Xbox Live dashboard. |
| DNS redirection | The module must let the guest resolve Xbox Live domains to Insignia's servers (e.g. primary DNS `46.101.64.175`). |

### Architecture

Emulate a virtual NIC standing in for the Xbox's **nVidia nForce MCP** network
adapter, with two selectable routing backends:

**a) PCap / bridged (desktop Windows/Linux)**
Injects the guest's raw Ethernet frames straight into the host's physical
adapter via Npcap/WinPcap.
* Best for LAN play and talking to real physical Xbox consoles.
* Fails on Wi-Fi adapters (they generally reject spoofed MACs) and needs
  root/admin.

**b) SLiRP / user-mode NAT (the Android path)**
SLiRP acts as a virtual router inside the emulator: it reads the guest's raw IP
packets, translates them into ordinary socket calls (`connect()`, `send()`,
`recv()`), and hands them to the host OS.
* Handles NAT traversal itself and reaches Insignia over plain TCP/UDP.
* **This is the one that matters for mobile**, because it needs no raw sockets.

> **Android note.** Android heavily restricts raw socket creation, so embedding
> a lightweight **lwIP** or **SLiRP** stack in C/C++ behind JNI is the effective
> route — it avoids requiring either root or Android's `VpnService`.

---

## 2. Xbox 360 module (Xenia-AE / Canary AE)

Connecting to official Xbox Live is impossible (missing security tokens), so the
module targets **System Link netplay**, tunnelled globally via XLink Kai.

### What the services require

| Requirement | Detail |
|---|---|
| UDP port 3074 | Xbox 360 System Link relies entirely on UDP 3074 for peer discovery and game traffic. |
| IPsec decryption | 360 network traffic is IPsec-encrypted. The module must derive the `XboxLANKey` (exported from the kernel) and the game-specific LAN key (from the XBE header) to decrypt packets before routing. |
| Title servers (optional) | Leaderboards/matchmaking outside System Link can be routed to custom REST APIs such as Xenia Web Services. |

### Architecture — HLE, not a virtual NIC

The 360's networking stack is complex enough that emulating the physical card is
the wrong level. Use **high-level emulation** instead:

1. **HLE socket interception.** Intercept the guest OS's XDK `socket()`,
   `sendto()` and `recvfrom()` calls at the kernel level, rather than
   translating raw Ethernet frames.
2. **Cryptography bypass.** Behind a config flag (conventionally
   `xlink_kai_systemlink_hack`): strip IPsec headers from outbound System Link
   packets and re-encrypt inbound ones before handing them back to the guest, so
   the payload is readable by standard P2P tunnelling software.
3. **Loopback relay bridge.** Broadcast the decrypted UDP to a local loopback
   address (`127.0.0.1:3074`); a secondary client — `kaiLoopbackBridge` or a
   background service built into the emulator — listens there and forwards
   across the internet to other emulator users.

---

## Notes for when this starts

* Follow the project rule: every piece ships behind its own toggle, default OFF,
  as a self-contained module that can be added, removed or modified freely.
* The `xlink_kai_systemlink_hack` flag belongs in per-game config, alongside the
  existing per-game settings pipeline.
* **Never bundle an `eeprom.bin`** — it is per-console identity, and sharing one
  gets users banned. It must be user-supplied, like the BIOS/boot ROM.
* Neither generation's plan depends on the other; the 360 side is pure HLE and
  touches none of the OG Xbox NIC work.
