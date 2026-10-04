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
| 1 | Repo & toolchain | ✅ done (chunk 2 deferred to Phase 9, D8) | G1 |
| 2 | Userspace reference driver (`rpcprobe`) | ✅ done (id 161 still unnamed) | G2a RX ✅, G2b TX ✅ |
| 3 | Kernel transport module | ✅ done (hard-hang reset untested) | G3 ✅ |
| 4 | brcmsmac fork + RPC stub layer | ⏳ | G4 |
| 5 | RX path + monitor mode | ⏳ | G5 |
| 6 | TX path + scan + open association | ⏳ | G6 |
| 7 | WPA2 + real-network use | ⏳ | G7 |
| 8 | Robustness & soak | ⏳ | G8 |
| 9 | Packaging & release | ⏳ | G9 = SPEC §9 |

**Chunk log** (newest first; one line per chunk, with what changed and where the notes are):

- 2026-10-04 · phase 3 · `driver/` `brcmsmac_usb.ko` (bmu_usb.c, bmu_rpc.c, bmu_debugfs.c): probe/disconnect, ROM-vs-running detection, RDL download via `request_firmware`, HELLO/CONNECT with the firmware version table, atomic-safe one-way calls, serialised sync calls (reply via request 9), bulk-IN URB pool with deaggregation and dispatch, debugfs (state/stats/trace/rpc/fault), register-access guard before BMAC_INIT, hang detection → port reset (≤3 per 10 min). Dev box (D11) loop `scripts/dev.sh`. **G3 passed** on x86: identity logged (chip 0x4322, fifos, NVRAM `manfid=0x846…`, MAC); 50/50 reloads (all reconnect to running firmware), 50/50 software replugs (sysfs `authorized`), 3/3 fault-injected reset recoveries; no oops/WARN/hung task, slab flat. Pi: armv6 cross-build loads/unloads (dongle not attached there). Physical replug cycles not automated (needs a human); real hard-hang reset untested (see docs/recovery.md).

- 2026-10-04 · phase 2.4–2.6 · `tools/rpcprobe/tx.py` (d11txh/PLCP/probe builders, tx status parser; reproduce the vendor's captured probe, auth and assoc-request headers byte for byte) + `tx` CLI command + 11 unit tests (`tests/test_tx.py`). **G2b TX passed** on ch 6: 2 broadcast + 2 directed probes, all tx statuses back in 5–6 ms, both unicast probes ACKed, 8 probe responses to our MAC from 3 APs. TXFIFO layout corrected (6 arg words + len); DOTXSTATUS fields decoded (frameid/status/sequence, brcmsmac TX_STATUS_* bits). IDs: 157 = PHY_LDPC_SET (position in every bring-up), 153 = CCA_STATS_READ, 154 = ANTSEL_SET [I]; +1 rule confirmed only up to 142; 161 unnamed (returns 1). `tools/rpcids.py` overrides. Power (unpowered USB-C hub port): undervoltage episodes 08:09:36–46 (spanning the whole TX window) and 08:11:47–51 (radio idle on channel, no host activity); no hang or disconnect, results unaffected. Notes in `docs/protocol.md` §5, §7.1, §7.4.
- 2026-10-04 · phase 2.1–2.3 · `tools/rpcprobe` library (transport/agg, session, sync, async reader, XDR, rx parser, JSONL trace, replay engine, CLI) + 11 unit tests on sanitised fixtures (`tests/`); `tools/seqextract.py`, `tools/mkfixtures.py`. **Init replay** of the vendor sequence (279 calls through HW_UP → BMAC_INIT → UP_FINISH) completes on hardware: 45/51 sync replies byte-identical, 6 in documented volatile classes, async frames identical to the capture. **G2a RX passed** (23 BSSs on ch 1/6/11, 1884 frames all FCS-valid, RxChan = DS IE for 23/23). Notes in `docs/protocol.md` §6, §7.2, §8.
- 2026-10-04 · phase 1 · git repo with local commits; tools promoted to `tools/` (neutral message-kind labels, `rpcids.py`, `usbrecover.py`); clean-room `docs/protocol.md`; `scripts/build.sh` (pi via Docker, native, x86) + `scripts/deploy.sh` verified with `driver/hello` on the Pi; recovery findings in `docs/recovery.md` (hard hang needs a physical replug on the Pi B+; firmware can also self-reboot). Chunk 2 (firmware extraction tool) **blocked**, awaiting a user decision.
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

**G1 status (2026-10-04):** ✅ cross-built (`scripts/build.sh pi`) and native hello modules each
loaded/unloaded twice via `deploy.sh`; x86 compile check passes; `git status --ignored` clean.
⏸ **Chunk 2 deferred to Phase 9** by user decision (D8): no extraction tool yet; development uses
the firmware in the private `work/` area. How users obtain firmware is decided at release time.

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

**Phase 2 status (2026-10-04):**
- 2.1 ✅ library + CLI (`python3 -m rpcprobe info|probe|up|rx|sync|call`), unit tests pass (`python3 -m unittest discover -s tests`).
- 2.2 ✅ init replay clean: 279 calls, 0 unexplained divergences. Volatile classes found and documented: REVINFO words 23–30 (uninitialised SRAM tail), DOIOVARS iovar 3, REG_READ 0x65a counter, id 153 stats. No register access before BMAC_INIT (guarded).
- 2.3 ✅ **G2a passed**: 23 BSSs on ch 1/6/11 in 30 s; every DS-channel IE within the tuned channel and equal to RxChan; 3 SSID+channel matches with the Mac's scan; RSSI vs the same dongle on the Windows PC within ±10 dB for 16/19 BSSIDs (different locations — RSSI parity is approximate). Needed one deviation from the capture: re-assert `MCTRL` `MCTL_BCNS_PROMISC` after each retune, because the vendor's last scan block (our ch 11 block) clears it at end-of-scan.
- Next: 2.4–2.5 TX (probe request via TXFIFO, DOTXSTATUS, G2b), then 2.6 unknown ids (153 looks like a stats/counters poll; 157 is a one-way call after UP_FINISH; 161 sync during early init returns 1).

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
| D8 | **Firmware distribution deferred to Phase 9** (user decision 2026-10-04); development uses `work/` firmware | Doesn't block Phases 2–8; decide with the full picture at release | user decides earlier |
| D9 | Userspace replay is **data-driven from private captures** (`seqextract` → `work/seq/`); only extractors and sanitised fixtures are committed | Keeps vendor-derived sequences private; the kernel driver must instead generate calls from brcmsmac logic (Phase 4) | — |
| D10 | Userspace TX builders mirror the vendor's d11txh for CCK management frames; the kernel driver builds headers with brcmsmac's own `brcms_c_d11hdrs_mac80211()` and uses the userspace builders only as a test oracle | brcmsmac already produces this exact layout; one source of truth | the RPC path needs fields brcmsmac doesn't fill |
| D11 | **x86 dev box** (MacBookPro5,5, Arch `linux-lts` 6.18, EHCI) is the primary target for Phases 3–8; the Pi is validated at major gates (G3, G4, G7) | Native builds, stable power, faster debugging; SPEC N1 already requires x86_64. The box's only uplink is its internal PCIe BCM4322 on the proprietary `wl` driver: our module binds only `0846:9020`, never touches `wl`/`cfg80211`/`b43`/`ssb`/`bcma`/`brcmsmac`, and associated test interfaces live in a network namespace | the Pi exposes ARMv6-only bugs |

## Risks

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| brcmsmac `main.c` merged HIGH+LOW too deeply to split cleanly | high | high | Phase 4.2 split map before coding; fall back to porting staging `wlc_mac80211.c` HIGH paths where needed |
| Unknown/unsupported RPCs or argument formats assert the firmware | medium | medium | Phase 2 replay-first; one change at a time; replug/USB reset recovery |
| RPC ID mapping drift above ID 142 | **observed** (shift grows to +3/+4; id 161 new) | medium | `tools/rpcids.py` overrides by behaviour; any stub for an id > 142 needs capture evidence first |
| Key-install semantics wrong (WPA2) | medium | high | capture-based PTK verification before Phase 7 |
| Pi 1 CPU too slow (RPC per frame) | medium | low | throughput isn't a goal; x86 box for dev speed |
| Pi PSU undervoltage with radio active | **observed** (Phase 2 RX and TX sessions; plug-in; `get_throttled` 0x50005 right after a TX session on an unpowered USB-C hub port) | medium | log `get_throttled` + undervoltage count around every hardware session; label coinciding failures POWER-SUSPECT; powered hub or ≥2.5 A supply before soak tests (Phase 8) |
| Captured blocks encode vendor *state machine* context (e.g. end-of-scan clears beacon-promisc) | medium | medium | treat captured blocks as references, not recipes; Phase 4 drives calls from brcmsmac state |
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
