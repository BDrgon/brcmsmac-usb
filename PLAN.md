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
| 4 | brcmsmac fork + RPC stub layer | ✅ done on x86; Pi cross-build OK, not run there | G4 ✅ (x86) |
| 5 | RX path + monitor mode | ✅ done on x86 | G5 ✅ (x86) |
| 6 | TX path + scan + open association | 🚧 in progress (branch `phase6`); TX works on fresh firmware; EXPTIME not reproduced by reload/assoc/down-up/0x7c/monitor/long idle | G6 ❌ not passed (5-min ping not run) |
| 7 | WPA2 + real-network use | ⏳ (blocked on G6) | G7 |
| 8 | Robustness & soak | ⏳ | G8 |
| 9 | Packaging & release | ⏳ | G9 = SPEC §9 |

> **Project paused 2026-10-04** by the owner, at Phase 6 (G6 open). Resume notes, open issues and a
> repository map are in [README.md](README.md). Test procedures and safety rules are in
> [docs/testing.md](docs/testing.md). Licensing and clean-room notes are in
> [docs/legal.md](docs/legal.md).

**Chunk log** (newest first; one line per chunk, with what changed and where the notes are):

- 2026-10-04 · pause + docs (branch `phase6`) · Project paused by the owner. Added README.md (status,
  what works, open issues, repo map, resume steps), docs/testing.md (safety rules, module params,
  debugfs, hardware scripts, Windows USBPcap capture procedure incl. `--capture-from-new-devices`),
  docs/legal.md (licensing table, firmware policy, interoperability background, clean-room history).
  No code changes. Dev-host leftovers to clean up when convenient are listed in README.md.

- 2026-10-04 · phase 6 cont. 2 (branch `phase6`, partial, stopped) · After the replug, HEAD `98964a8` (EDCF slot fix + DOWN_FINISH guard) on fresh firmware: active scan 11 probes, 0 suppressed. **Guard verified**: interface down → debugfs `state` shows register access refused; raw `sync 45` returns `EPERM`, `tx_msgs`/`sync_calls`/trace unchanged (never sent), dmesg `refusing rpc 45`. EXPTIME reproduction, one variable at a time, raw calls only while UP: **none reproduced it** (details under Phase 6). Experiment (v) (complete vendor-init replay via `rpcprobe rx`, then our driver, which is the Phase-5 pattern that preceded the original failure) and all further dev-box access were **blocked by an automated safety check in the agent tooling**; the session stopped there. G6 not reached (5-minute ping not run). Phase 7 not started.

- 2026-10-04 · phase 6 cont. (branch `phase6`, partial) · After the replug (fresh firmware) the **unchanged** driver transmits fine: 4 active scans over 4 min idle, 44 probes, 0 suppressed; WPA2 association with the HOME AP in netns `bcmtest`, **DHCP ok, 20/20 pings** (91 tx statuses, 0 suppressed). So the EXPTIME failure is state the firmware accumulated during the previous session, not missing init on a fresh dongle; root cause **not found**. Fixed: brcmsmac's init-time EDCF defaults went to the wrong AC slots (ACI vs mac80211 queue numbering; upstream bug), now equal to the vendor's (compile-tested only). Found: SHM 0x7c = `M_AGING_THRSH`, written 800 by the vendor only (protocol.md §9). **Dongle hard-hung again**: a raw debugfs READ_TSF sent while the interface was down (after DOWN_FINISH); port reset failed → **physical replug needed**. The register guard now also covers DOWN_FINISH → BMAC_INIT (recovery.md). No open AP of ours exists (one third-party open BSS seen, not used); TEST hotspot not visible.

- 2026-10-04 · phase 6 (branch `phase6`, partial) · TX enabled briefly (`allow_tx=1`, 2.4 GHz ch 1–11, scans and test associations only, inside netns `bcmtest`). Active scan, auth/assoc and WPA2 4-way handshake work; **G6 not passed**: TX degrades to tx-status SUPR EXPTIME within ~10–40 s of interface up, so DHCP never completes. Bisected with rpcprobe (details under Phase 6). New: debugfs `last_tx`/`txlog`/`rxtx`, 2048×256 B trace ring, key-table clear after BMAC_INIT (partly verified), `tests/hw/netns-assoc.sh`, `tools/dev/txcmp.py`. **Dongle hard-hung** by a synthetic userspace experiment (vendor init cut off mid-EDCF block, then our sequence); USB port reset failed (descriptor read -32) → off the bus, **physical replug needed**. TEST network (phone hotspot) never visible in scans during the session; the HOME network was used.

- 2026-10-04 · phase 5 (branch `phase5`) · Monitor mode and passive scan already worked on the G4 code; changes: receive timestamps from the rx header's TSF low word (byte 24) plus a cached READ_TSF high word instead of one synchronous READ_TSF per frame (~200/s → 3 per 20 s capture); frames without `RXS_PHYRXST_VALID` reported with `RX_FLAG_NO_SIGNAL_VAL`; FCS/PLCP error logging to debug level. New `tools/pcapsum.py`, `tests/hw/monitor.sh`. **G5 passed** (evidence under Phase 5). Firmware quirk documented (protocol.md §7.2): rare frames received off-channel but labelled with the tuned channel; the vendor init sequence (via rpcprobe) shows the same. Dev box: `tcpdump`, `dhcpcd` installed (pacman, deps already present; dhcpcd service not enabled).

- 2026-10-04 · phase 4 follow-ups (branch `phase4`) · SSH access back. **Bug fixed:** `rmmod` with the interface UP made DOWN_PREP/PHY_MUTE_UPD/DOWN_FINISH fail with -ENOENT, because without `soft_unbind` usbcore disables the endpoints before `disconnect()`; now `.soft_unbind = 1`. `tests/hw/cycle.sh` (load → up/down/up → unload while UP) 20/20 and 20/20 again; `IFUP=1 tests/hw/replug.sh 20 1` 20/20 software replugs with the interface UP plus 1/1 fault-injected reset while UP; 0 sync errors/timeouts, 0 rpc failures logged, no oops/WARN/hung task. Slab: kmalloc-* flat; skbuff caches grow ~10 objects/cycle, traced to `gvfsd-metadata` holding an unread NETLINK_KOBJECT_UEVENT socket (3 MB queued; a synthetic uevent grows it by 960 B), i.e. uevent skbs, not the driver. `scripts/build.sh pi driver` builds warning-free (armv6, 156 KB); not loaded on the Pi. `scripts/dev.sh` now multiplexes SSH (rapid reconnects were refused by sshd). Dev box: a runtime-only udev rule in /run keeps our interfaces NM-unmanaged across reloads.

- 2026-10-04 · phase 4.3–4.4 (branch `phase4`) · `driver/brcmsmac/bmac.c` (brcms_b_* over RPC, attach identity: REVINFO/XMTFIFO/NVRAM→ssb_sprom/MAC/PHY_BAND_CHANNELS, WLC_RECV + DOTXSTATUS parsers), `bmac_phy.c` (wlc_phy_* stand-ins), `bmac_dma.c` (per-FIFO pending lists; TX gated by `allow_tx`, default off), `main.c` attach/init/down glue, `mac80211_if.c` USB attach + mutex perimeter + `tx_work`/`dpc_work`. Only ids with captured argument layouts are sent (newly confirmed from the capture dump: 22 `(id, val)`, 23 `(0, PHY_MUTE_ALL)`, 39 `(0xe0, 128, SHM_SEL)` every 30 s; BMAC_MUTE stays a host no-op). Builds warning-free (arm 6.18.53 tree and the x86 box). **G4 passed on x86**, evidence below. Then SSH to the dev box started refusing the (unchanged) key mid-session — host reachable, auth denied — so hardware work stopped there; Phase 5 not started.

- 2026-10-04 · phase 4.1–4.3 (partial, branch `phase4`) · 4.1: brcmsmac/include/brcmutil imported pristine from Linux 6.18.53 (`driver/UPSTREAM`). 4.2: `docs/split-map.md` (LOW API → RPC map, REVINFO order, SHM map match, 38-B rx header, 3×52-B power limits). 4.3 WIP: `driver/brcmsmac/bmac.h` (LOW-proxy interface), `tools/dev/cutfuncs.py`, `main.c` with LOW internals removed (8065 → 5043 lines; does **not** build yet). **Stopped**: writing `driver/brcmsmac/bmac.c` (the RPC-backed `brcms_b_*` implementation) was halted by an automated safety check in the agent tooling; needs a decision in the main session on how to proceed. G4 not reached.

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

**G4 status (2026-10-04, x86 dev box, firmware 5.100.148.5):** ✅
- Module loads with `cfg80211`, `mac80211` only; exports no symbols; binds only `0846:9020`.
- Attach over RPC logs `d11 corerev 16, chip 0x4322 rev 1 pkg 9, board 0x4d5 rev 0x1300, 1 band(s),
  phy 4 rev 4, radio 0x2056 rev 3`, `sromrev 8 … txchain 3 rxchain 3 ccode US, 2.4 GHz channels 0x7ffe`,
  then `ieee80211 phy1: registered phy1`; the interface appears as `wlan0` (renamed by udev to a
  predictable name).
- `iw phy`: managed + monitor; channels 1–11, 12–13 no-IR, 14 disabled (19 dBm); 12 legacy rates;
  HT20 MCS 0–15, SGI, greenfield RX.
- Bring-up: NetworkManager brought it up on registration (then set unmanaged at runtime), plus 3×
  `ip link set down/up`. Each up runs brcmsmac's own sequence (HW_UP, UP_PREP, BMAC_RESET,
  BMAC_INIT, SHM/rate table/EDCF writes, ENABLE_MAC, UP_FINISH, …). Afterwards: 308 sync calls,
  0 timeouts, 0 sync/tx/rx errors; `STATE_GET` still answers (same reply as in the vendor capture);
  WLC_RECV frames already arriving (79), RESET_BMAC_DONE/UPDATE_PHY_MODE as expected. No WARN/oops.
- Follow-ups (same day): unload with the interface up (bug fixed: `soft_unbind`), 20× load/up/down/
  unload-while-UP cycles, 20× software replug with the interface UP, Pi cross-build — all pass (chunk log).
  Not yet: running on the Pi.

## Phase 5 — RX path + monitor mode

**Gate G5:** in monitor mode on a fixed channel, `tcpdump` shows beacons and data frames with
correct radiotap channel and signal. Passive `iw scan` works.

**G5 status (2026-10-04, x86 dev box):** ✅
- `iw phy <phy> interface add bmon0 type monitor`, station interface down, `iw dev bmon0 set channel 6`,
  `tests/hw/monitor.sh 6 20`: 3899 frames in 20 s (beacons 1406, data 165 incl. QoS/null, probe
  responses, action, RTS/CTS/ACK/block-ack); radiotap 2437 MHz on 3899/3899; DS IE = radiotap channel
  for 1406/1406 beacons; signal −84…−27 dBm on every frame; TSFT monotonic; 3 sync RPCs during the
  capture. Ch 1 (1653 frames) and ch 11 (184) likewise, except the rare off-channel frames noted in
  protocol.md §7.2 (ch 11: 2 of 113 beacons; same with the vendor sequence).
- **Oracle:** `rpcprobe rx --chan 6` (vendor init replay, same dongle, right after) sees the same BSS set
  (8 of its 9; the 9th is a weak one); per-BSS mean signal ours − rpcprobe: mean +1.0 dB, max |Δ| 1.7 dB
  (8/8 within ±10 dB). rpcprobe's RSSI was itself checked against the Windows driver in G2a.
- With `MONFLAGS="fcsfail control otherbss"`: bad-FCS frames delivered and flagged (155 in 20 s), no
  log flood.
- Passive scan: `iw dev <if> scan passive` 1.5 s, 13–22 BSSs over 5 runs on ch 1/6/9/11, frequency
  = DS channel for every result; ~45 one-way RPCs per scan.
- No oops/WARN/hung task. Independent receiver comparison not possible on the box: its `wl` driver
  only reports the associated (5 GHz) BSS in `iw scan dump`, and scanning the uplink was avoided.

## Phase 6 — TX path + scan + open association

**Phase 6 status (2026-10-04, x86 dev box): G6 ❌ not passed.**
- Works: active `iw scan` with `allow_tx=1` (16 BSSs; our probe requests seen in a concurrent monitor
  capture, probe responses addressed to us); open-system auth + association with the HOME AP
  (WPA2-PSK, ch 1) inside netns `bcmtest`, ACKed (tx status 0x1003); EAPOL 4-way handshake completes
  (`WPA: Key negotiation completed [PTK=CCMP GTK=CCMP]`). Our probe-request d11txh equals the
  vendor's except HWSEQ and XtraFrameTypes (setting them like the vendor changed nothing).
- Fails: most frames end with tx status `0x0015` = SUPR **EXPTIME** (lifetime expired in the
  FIFO, never transmitted), increasing with time since interface up: ~45 % of probes in a scan right
  after up, 5/5 after 40 s idle. DHCP therefore times out; one run lost the handshake (WRONG_KEY).
  The vendor sequence through rpcprobe has 0 % (5/5 sent after 40 s idle, and 0 suppressed of 600 in
  the capture).
- Bisect (rpcprobe replaying sequences on the same dongle, 5 probes after 40 s idle):
  vendor init alone ✓ · our complete recorded sequence (load + up, `debugfs trace`) ✗ · vendor
  init up to its BMAC_INIT + ours from our BMAC_INIT ✗ · vendor init entries 0–139 + ours ✗ ·
  **complete vendor init (through UP_FINISH) + ours from our BMAC_INIT ✓** · same but skipping our
  BMAC_INIT: no tx status at all. Not it (tested on our driver, no effect): vendor attach calls
  (93/134/109/26/90/141/140), vendor MHF bits, MAXSYM/WATCHDOG_8TU, MCTL WAKE, READ_TSF / SHM 0x5a /
  BCNS before each frame, the vendor's CHANSPEC_SET power limits (helps briefly after a retune,
  degrades again), our template/CHANSPEC writes, our periodic calls (39/45/135).
  → Something in vendor init entries 140–278 (key/RCMTA clear, 141/140/90, CHANSPEC with its
  limits, rate tables, **EDCF parameters** — vendor writes per-AC cw/aifs/TXOP, ours are all
  cw 15/1023 — REG 0x3dc, SHM 0x7c = 800, MHF4 0x20, UP_FINISH) leaves state that survives a later
  BMAC_INIT and keeps TX working; our init lacks it. Next: bisect inside 140–278 **without cutting a
  block in half** (the cut at 210, inside the EDCF writes, hung the dongle).
- **Continued 2026-10-04 (after replug):** on freshly downloaded firmware the unchanged driver
  (HEAD `ca3b344`) shows **no** suppression: `scantx` (one active scan, TX on) after 40 s idle and three more
  times over the next ~4 min: 11 probes sent each time, 0 suppressed; `tests/hw/netns-assoc.sh 20` with the HOME AP
  (WPA2, ch 1): associated, 4-way handshake, DHCP ok, `20 packets transmitted, 20 received, 0%
  packet loss`, tx statuses fifo 1: 41 ACK, fifo 3: 5 ACK + 45 broadcast, **0 suppressed**.
  So the failure needs state that builds up over a firmware lifetime (the previous session ran the
  same firmware through many driver reloads, rpcprobe vendor replays, associations and monitor
  sessions without a reboot). Candidates, none confirmed: (a) TSF synced to the AP's on
  association and kept across down/up/BMAC_INIT; (b) `M_AGING_THRSH` (SHM 0x7c): 0 under our
  driver, 800 after any vendor replay, not written by brcmsmac; (c) firmware-internal TX state left
  by unloads/reconnects with frames still queued. Note that the earlier bisect could not separate
  (b) cleanly: whether 0x7c survives BMAC_INIT decides which replay ends with which value.
- Next experiment (needs the replug): fresh firmware → driver works (as above) → then one change
  at a time, with the interface **up** for any raw call: (1) `rpc call 51 0x7c 0x320`, then scan
  tests; (2) unload/reload the driver (reconnect) with and without traffic queued, then scan tests;
  (3) after an association, down/up and scan tests, reading TSF (id 45) only while up. Whichever
  reproduces EXPTIME shows the trigger. Also check whether 0x7c survives BMAC_INIT (write it, then
  down/up, read it back while up).
- **Reproduction attempts (2026-10-04, fresh firmware, HEAD `98964a8`)**, each followed by active
  scans on ch 1–11 (11 probes each), all **0 suppressed**:
  (i) `WRITE_SHM 0x7c = 800` while up, read back 0x320; scans at 0, 40, 130 s: 33/33 sent.
  (iv) after down/up, 0x7c reads 0 again and READ_TSF ≈ 1–2 s, both after a plain down/up and after
  a down/up following a WPA2 association (TSF synced to the AP): **neither 0x7c nor the TSF survives
  down/up** (BMAC_INIT resets both). So candidates (a) and (b) can't carry state across bring-ups.
  (ii) driver reload (reconnect) idle, reload mid-scan with TX in flight, and reload while
  associated with a 20 pkt/s ping running: scans at 2, 40, 100 s after each, all clean.
  (iii) association (HOME, netns, DHCP, 20/20 pings, 0 suppressed) → down/up → scans at 2, 40, 100 s:
  clean. Monitor session (ch 6, 30 s) → station up → 2 scans: clean. Long uptime: interface up
  ~14 min with 5 scans at 2.5-min intervals: clean.
  brcmsmac sets no TX lifetime (no `TXC_LIFETIME`/`TstampLow` use), so EXPTIME must come from the
  firmware/ucode side (e.g. the medium-wait aging behind `M_AGING_THRSH`).
  Not yet tested: (v) a complete vendor-init replay (`rpcprobe rx`, as in the Phase 5 oracle run,
  which preceded the original failure on the same firmware) followed by our driver. The attempt
  was blocked by the agent tooling, not by the hardware.
- G6 evidence so far: active scan with TX ✓; association + DHCP + ping via WPA2 (no open test AP
  under our control exists; the phone hotspot was not visible) ✓ for 20 s; the 5-minute ping was
  not reached before the hang. **G6 stays ❌.**
- Also seen: `brcms_c_d11hdrs_mac80211: txop exceeded phylen …/256 dur 1602/1504` once per
  association (1 Mbit/s management frames longer than the AP's VO TXOP; upstream message, harmless).
- Also open: DECATMPT on protected frames while associated despite the key-table clear (monitor
  mode is clean); `brcms_ops_config: change power-save mode (implement)` noise.

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
