# brcmsmac-usb — PLAN

How we get to [SPEC.md](SPEC.md). Work moves in **phases**, each ending in a **gate**: a concrete,
testable result that must pass before the next phase starts. Work is done in **chunks**, one
subagent at a time. Each chunk takes one bounded piece of a phase, leaves the repo better than it
found it, and updates the status board below.

---

## Status board

| Phase | Name | State | Gate |
|---|---|---|---|
| 0 | Reconnaissance | ✅ done | firmware extracted, protocol identified, capture decoded |
| 1 | Repo & toolchain | ⏳ next | G1 |
| 2 | Userspace reference driver (`rpcprobe`) | ⏳ | G2a RX, G2b TX |
| 3 | Kernel transport module | ⏳ | G3 |
| 4 | brcmsmac fork + RPC stub layer | ⏳ | G4 |
| 5 | RX path + monitor mode | ⏳ | G5 |
| 6 | TX path + scan + open association | ⏳ | G6 |
| 7 | WPA2 + real-network use | ⏳ | G7 |
| 8 | Robustness & soak | ⏳ | G8 |
| 9 | Packaging & release | ⏳ | G9 = SPEC §9 |

**Chunk log** (newest first; one line per chunk, with what changed and where the notes are):

- 2026-10-04 · scaffolding · SPEC/PLAN written; repo skeleton, `.gitignore`, `config/local.env.example`.
- 2026-10-04 · milestone 2 · read-only RPC calls pass on Pi (`work/tools/m2.py`); REG_READ before BMAC_INIT hangs the dongle.
- 2026-10-04 · capture decode · run2 decoded; ID mapping 5.75→5.100 (≤61 same, ≥65 +1); RX/TX layouts (`work/PROTOCOL.md`).
- 2026-10-04 · milestone 1 · RDL download + CONNECT handshake from Python on the Pi.
- 2026-10-04 · recon · firmware extracted from vendor driver; BMAC split-MAC identified.

---

## Phase 0 — Reconnaissance ✅

Already done; see `work/REPORT.md` and `work/PROTOCOL.md`. Known facts:

- **Firmware download (RDL):** vendor control requests 0/2/3/4/5/8, bulk download chunks.
  `DL_GETVER` returns `0x4322` from the boot ROM and `0xA123` once firmware is running.
- **Transport:** each RPC frame is `u32le tp_len` followed by the frame. Host→dongle frames go on bulk
  EP 0x03, with DATA frames aggregated (up to 3 per transfer). Async dongle→host frames arrive on bulk
  EP 0x82, also aggregated. **Returns to RTN calls are collected with control IN `bRequest 9`,
  `wLength 1928`.**
- **RPC header:** `type<<24 | session<<16 | xid`. Handshake: MGN HELLO, then CONNECT, which must
  carry the exact firmware version (`0x05649405`).
- **RPC IDs:** 5.100 = 5.75 enum for IDs ≤61, and +1 for IDs ≥65. IDs 153, 157 and 161 are unknown.
- **RX:** `WLC_RECV` (81), carrying a 38-byte rxhdr, PLCP and 802.11 frame. **TX:** `TXFIFO` (61),
  carrying 6 words, length, a 112-byte d11txh, PLCP and the frame. The hardware does CCMP.
- **Hazard:** register access before `BMAC_INIT` hangs the whole dongle; recovery is a replug or USB reset.

## Phase 1 — Repo & toolchain

**Goal:** a shareable repo with a reproducible build and a one-command test loop on the Pi.

Chunks:
1. `git init`; small, logically-scoped local commits. Promote stable tools from `work/tools/` into `tools/`: `rdl`, `usbpcap`, `rpcdecode`,
   `idtable`, and `m2` → `tools/rpcprobe/`. Strip local paths, and read local settings from `config/local.env`.
2. Write `tools/fwextract`. Input: the user's vendor zip. It unpacks the installer (7z/unshield),
   locates the TRX by header scan plus a version string, and writes `brcm/bcm4322-bmac-5.100.148.5.trx`.
   It verifies the SHA-256 against `tools/fwextract/known-firmware.tsv`, which holds hashes only.
   Prefer pure Python plus documented external tools, and say clearly what it needs.
3. Write an **independent protocol spec**: rewrite `work/PROTOCOL.md` into `docs/protocol.md` using
   only capture evidence and ISC headers (clean-room; see SPEC §6). Remove any reference to the
   proprietary router sources.
4. Set up a cross-compile environment on the Mac: a Docker image with an armv6 toolchain plus the
   kernel headers/build tree for the Pi's exact kernel (`uname -r` on the target). Also an x86_64
   build target. Script: `scripts/build.sh [pi|x86]`.
5. Deploy loop: `scripts/deploy.sh`, which copies the `.ko` to the Pi, `rmmod`, `insmod` and tails
   `dmesg`. A replug-free recovery path: USB reset via `usbreset`/sysfs `authorized`, and per-port
   power if the hub supports it (Pi B+ LAN9514: check `uhubctl`).

**Gate G1:** a hello-world module built on the Mac loads on the Pi, and `scripts/deploy.sh` is
idempotent. `fwextract` reproduces the known hash from the vendor zip. `git status` shows no
firmware, captures, vendor files or local config.

## Phase 2 — Userspace reference driver (`tools/rpcprobe`)

**Goal:** prove every protocol piece the kernel driver needs, cheaply, in Python on the Pi, before
writing kernel code. It also becomes a permanent debugging and test oracle.

Chunks:
1. Library: transport (aggregation both ways), sessions, sync calls, async reader thread, XDR
   helpers, an ID table loaded from a data file, and a trace log.
2. **Replay init:** reproduce the Windows init sequence from the run2 capture (PROTOCOL §6) up to and
   including `BMAC_INIT` and `HW_UP`. Do it call by call, comparing return values with the capture.
   Treat any divergence as a stop-and-investigate point.
3. **G2a RX:** set the channel (CHANSPEC_SET), enable the MAC, and parse `WLC_RECV`. Print beacons
   with SSID, BSSID, RSSI and channel. Cross-check against a scan from another device.
4. **TX:** build a probe request (d11txh + PLCP + frame) modelled on the captured ones, send it via
   `TXFIFO`, and read `DOTXSTATUS`.
5. **G2b TX:** a directed probe request to the test AP gets a probe response addressed to our MAC.
6. Resolve the unknown IDs 153/157/161 by experiment and by capture context. Write down the answers.

**Gate G2a:** at least 3 known APs are seen with the correct SSID and channel, and RSSI is within
±10 dB of another device. **Gate G2b:** a probe response to our MAC is received, and the tx status
reports an ACK.

## Phase 3 — Kernel transport module

**Goal:** `usb.c` + `rpc.c` as a standalone module (no mac80211 yet) that reaches the milestone-2
level of capability in kernel space.

Chunks:
1. USB probe/disconnect for `0846:9020`. Detect ROM vs running firmware. RDL download from
   `request_firmware("brcm/bcm4322-bmac-5.100.148.5.trx")`. Re-probe handling after `DL_GO`.
2. RPC core: HELLO/CONNECT, version check against the firmware table, xid management, and sync RTN
   calls (bulk OUT + ctrl req 9, with timeout and serialisation). Async RX via an anchored bulk-IN
   URB pool, with deaggregation and a dispatch table.
3. debugfs: `rpc_raw` (write a hex frame, read the reply), `stats`, and `trace` (ring buffer of the last 256 frames).
4. Hang detection: RPC timeout → `usb_reset_device()` → full re-init. Rate-limited.

**Gate G3:** loading the module prints revinfo, NVRAM `manfid` and the MAC. 50× `rmmod`/`modprobe`
and 50× unplug/replug cause no oops, leak or hung task (`CONFIG_DEBUG_*` kernel optional on the x86 box).

## Phase 4 — brcmsmac fork + RPC stub layer

**Goal:** the upper MAC compiles against the RPC layer and registers an `ieee80211_hw`.

Chunks:
1. Import `brcmsmac` from the **target kernel's exact tree** (rpi-6.18.y) into `driver/`, keeping
   the ISC headers. Record the upstream commit in `driver/UPSTREAM`.
2. **Touch-point inventory:** list every `bcma_*`, `brcms_b_*`, `wlc_phy_*`, `dma_*` and register
   accessor reachable from the HIGH code. Classify each one: RPC id / host-only / delete. Output:
   `docs/split-map.md`. This is the biggest design artefact in the project.
3. Write `bmac_rpc.c` with one stub per needed id, generated from the ID table where possible. Remove
   `phy/`, `dma.c` and the bcma glue from the build.
4. Probe path: transport up → init sequence (same as G2) → `ieee80211_register_hw` with the MAC from
   `HW_ETHERADDR`, plus bands and channels.

**Gate G4:** `iw phy` shows the device with sensible 2.4 GHz channels and rates. `ip link` shows
`wlanN`. Bringing the interface up doesn't assert the firmware.

## Phase 5 — RX path + monitor mode

**Gate G5:** in monitor mode on a fixed channel, `tcpdump` shows beacons and data frames with
correct radiotap channel and signal. Passive `iw scan` works.

## Phase 6 — TX path + scan + open association

**Gate G6:** active `iw scan` lists APs. The interface associates with an **open** test AP (for
example hostapd on a Linux laptop, or a phone hotspot if it can run open) and pings the gateway for 5 min.

## Phase 7 — WPA2 + real-network use

Chunks include confirming the key-install path: the capture showed two 16-byte `COPYTO_OBJMEM` (40)
writes during association. Verify them by deriving the PTK from a capture made with a known
passphrase.

**Gate G7:** `wpa_supplicant` joins the test WPA2-PSK network, DHCP works, and a 1 h ping soak
passes. Then it joins the home network and serves as the Pi's home-side link, with `eth0` kept as
the fallback/management path during the trial.

## Phase 8 — Robustness & soak

**Gate G8:** SPEC §9 items 3–4: a 24 h soak, an `iperf3` baseline, 50× replug and reload cycles,
recovery from forced firmware hangs (`debugfs` fault injection), and undervoltage events logged
without a crash.

## Phase 9 — Packaging & release

DKMS (`dkms.conf`), README (getting the vendor package, `fwextract`, install, troubleshooting),
LICENSE, `docs/decisions.md`, a sanitised-fixtures audit, and an x86_64 verification.
**Gate G9:** SPEC §9 passes for a "fresh user" walk-through.

---

## Decisions (ADR-lite)

| # | Decision | Why | Revisit if |
|---|---|---|---|
| D1 | Fork mainline **brcmsmac** (ISC), not the 2.6.37 staging driver | Modern mac80211 API, maintained code; staging is only used as an ID/semantics reference | brcmsmac's HIGH/LOW merge proves impossible to split |
| D2 | Primary firmware **5.100.148.5** | It's what Windows loads; we have a full capture of it and the ID mapping | a 5.60 capture becomes available and is simpler |
| D3 | Prove each protocol piece in **Python first** (Phase 2) | Kernel debugging on a Pi 1 is slow; a userspace oracle catches mistakes early | a piece is kernel-only (e.g. URB timing) |
| D4 | **Out-of-tree + DKMS**; no upstream submission planned | Proprietary firmware and RE'd protocol make upstream unlikely | interest emerges |
| D5 | **Clean-room** from proprietary router sources | Licence (SPEC §6) | — |
| D6 | Firmware filename `brcm/bcm4322-bmac-<version>.trx`; version table in the driver | Lets several firmware versions coexist; explicit | — |
| D7 | Local setup in `config/local.env` (gitignored) | Shareability | — |

## Risks

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| brcmsmac `main.c` merged HIGH+LOW too deeply to split cleanly | high | high | Phase 4.2 split map before coding; fall back to porting staging `wlc_mac80211.c` HIGH paths where needed |
| Unknown/unsupported RPCs or argument formats assert the firmware | medium | medium | Phase 2 replay-first; one change at a time; replug/USB reset recovery |
| RPC ID mapping drift beyond ID 152 | medium | medium | experiments in Phase 2.6; more captures |
| Key-install semantics wrong (WPA2) | medium | high | capture-based PTK verification before Phase 7 |
| Pi 1 CPU too slow (RPC per frame) | medium | low | throughput isn't a goal; x86 box for dev speed |
| Pi PSU undervoltage with radio active | medium | medium | monitor `get_throttled`; powered hub; better PSU |
| Hidden firmware expectations (watchdogs, keepalives) | medium | medium | look for periodic RPCs in the idle portion of the capture; replicate them |
| Legal (firmware/RE) | low | high | SPEC §6 policy; users supply vendor files |

## How chunks are run

- **One subagent at a time.** Each gets: the SPEC, this PLAN, the phase and chunk IDs, and the
  constraints below. It returns a summary, updates this file's status board and chunk log, and writes
  notes under `docs/` (shareable) or `work/notes/` (private).
- **Shared constraints for every chunk:**
  - Never commit or copy firmware, vendor files, captures or proprietary sources.
  - Settings come from `config/local.env`.
  - **Pi safety:** never disrupt `eth0`/SSH. Kernel modules are loaded only by the deploy script, and
    only one at a time. Check `vcgencmd get_throttled` before and after hardware tests. Recover by
    USB reset or by asking the user to replug.
  - **No kernel-wide changes on the Pi** (no blacklists, no `/etc/modules`) until Phase 9.
  - **Windows box:** captures only; don't install anything new without saying so.
  - Local commits and branches allowed (approved 2026-10-04); no push/remote/publish until the user has reviewed.
- **Gate reviews** happen in the main session with the user before the next phase starts.
