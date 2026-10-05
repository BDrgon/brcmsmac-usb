# brcmsmac-usb

A Linux mac80211 driver for the **Netgear WNA3100 v1** USB Wi-Fi adapter (USB `0846:9020`, Broadcom
BCM4322 "BMAC" split-MAC). The adapter has never had a Linux driver. This project adapts the
in-kernel, ISC-licensed `brcmsmac` driver to drive the adapter's own firmware over USB RPC.

> **Status: paused (2026-10-04).** Receive works end to end. Transmit works on freshly booted
> firmware: active scan, WPA2 association, DHCP and ping all succeed. Gate G6 is still open
> because of an intermittent transmit failure ("lifetime expired") that hasn't been reproduced.
> Development was paused by the project owner; see [Resuming](#resuming).

## How it works

| Half | Runs on | Contains |
|---|---|---|
| LOW (`wlc_bmac`) | the adapter's ARM core, firmware downloaded at plug-in | MAC core, PHY/radio, calibration, DMA, crypto engine |
| HIGH | the host: this driver | 802.11 MAC (mac80211 + brcmsmac upper layers) |

The two halves talk over a USB RPC protocol. It is documented in [docs/protocol.md](docs/protocol.md),
which was derived from our own USB captures and ISC-licensed headers. Wherever brcmsmac would touch
PCI hardware directly (registers, DMA, PHY), this driver sends the equivalent RPC instead. The full
function-by-function map is [docs/split-map.md](docs/split-map.md).

**Firmware is never distributed.** Users obtain it from their own copy of the vendor's Windows
driver package. How the user-run extraction tool will work is still undecided (PLAN decision D8,
deferred to Phase 9).

## What works (x86, linux-lts 6.18, firmware 5.100.148.5)

| Gate | Result |
|---|---|
| G1 toolchain | Cross-builds for Raspberry Pi (armv6) in Docker, native x86 builds, deploy scripts |
| G2a/G2b userspace | Python reference driver (`tools/rpcprobe`): vendor init replay, beacon RX, probe TX with ACKs |
| G3 transport | `brcmsmac_usb.ko`: firmware download, RPC session, sync/async calls, debugfs; 50× reload, 50× software replug, recovery from injected faults |
| G4 mac80211 | Registers as a 2.4 GHz HT20 phy; interface up/down; `rmmod` with the interface up; 40 load/unload cycles and 20 replugs with the interface up |
| G5 RX/monitor | Monitor mode: radiotap channel/signal correct on every frame (3,899 frames in 20 s on ch 6), signal within 1.7 dB of the reference driver; passive scan |
| G6 TX | **Open.** On fresh firmware: active scan (0 suppressed probes), WPA2 association, 4-way handshake, DHCP, 20/20 ping, all inside a network namespace. The 5-minute ping for the gate hasn't been run yet. |
| G7–G9 | Not started (WPA2 soak, robustness/soak on the Pi, packaging) |

Branches build on each other: `main` → `phase2` → `phase3` → `phase4` → `phase5` → `phase6`
(latest). Each phase's gate evidence is in [PLAN.md](PLAN.md).

## Known open issues

1. **TX "lifetime expired" (tx status 0x0015).** It appeared once, late in a long session where the
   same firmware instance had run through many reloads and vendor-sequence replays. It has not
   reproduced on fresh firmware. The following were ruled out: SHM 0x7c (`M_AGING_THRSH`), TSF
   carry-over, reload with traffic queued, association → down/up, monitor sessions, and 14 min of
   uptime. Untested lead: a complete vendor-init replay with `rpcprobe`, followed by this driver,
   on the same firmware instance. Details are in PLAN.md (Phase 6).
2. **Hard hangs need a physical replug.** Register, SHM or TSF access while the MAC core is down
   (before `BMAC_INIT` or after `DOWN_FINISH`) hangs the adapter, and a USB port reset does not
   recover it on either host tested. The driver refuses such calls (verified on hardware). It does
   not yet guard MCTRL/ENABLE_MAC/SUSPEND_MAC/SET_ADDRMATCH, which also touch the core; none has
   been seen to hang it. See [docs/recovery.md](docs/recovery.md).
3. While associated, the hardware still attempts decryption of protected frames (DECATMPT flag), even
   though mac80211 does the crypto in software. Traffic isn't affected.
4. brcmsmac logs "(implement)" noise for power-save/QoS paths that have no RPC equivalent yet.
5. Nothing has been run on the Raspberry Pi beyond cross-build and load/unload. The Pi used here has
   a marginal power supply (undervoltage logged).
6. RPC id 161 is unnamed. Ids above 142 are mapped by observed behaviour, not by a rule.

## Repository map

| Path | What |
|---|---|
| [SPEC.md](SPEC.md) | Fixed target: requirements, architecture, licensing policy, definition of done |
| [PLAN.md](PLAN.md) | Phases, gates, status board, chunk log, decisions, risks |
| [docs/protocol.md](docs/protocol.md) | Wire protocol: RDL download, framing, RPC header, handshake, ids, RX/TX formats |
| [docs/split-map.md](docs/split-map.md) | brcmsmac LOW/HIGH split: which call becomes which RPC |
| [docs/recovery.md](docs/recovery.md) | Failure modes, hang triggers, recovery behaviour per host |
| [docs/development.md](docs/development.md) | Build, deploy and dev-box loop; rpcprobe usage |
| [docs/testing.md](docs/testing.md) | Hardware test scripts, debugfs, safety rules, USB capture procedure |
| [docs/legal.md](docs/legal.md) | Licensing, firmware policy, interoperability notes, clean-room history |
| `driver/` | `bmu_*.c` transport; `brcmsmac/` adapted upper MAC (`bmac*.c` = RPC-backed LOW layer) |
| `tools/` | `rpcprobe` reference driver, capture decoders, id tables, recovery helper |
| `tests/` | unit tests (`pytest tests`) and `tests/hw/` hardware scripts |
| `config/local.env.example` | Template for installation-specific settings (`local.env` is gitignored) |

## Resuming

1. Read PLAN.md's status board and the latest Phase 6 chunk-log entries.
2. Plug the adapter into the dev host. It enumerates in boot-ROM state, and the driver downloads the
   firmware itself (`/lib/firmware/brcm/bcm4322-bmac-5.100.148.5.trx`, extracted by the user).
3. `scripts/dev.sh load`, bring the interface up, run an active scan with `allow_tx=1`, and check
   for 0 suppressed frames.
4. Finish G6: `WPA_CONF=… tests/hw/netns-assoc.sh 300` (5-minute ping inside netns `bcmtest`).
5. Then Phase 7 (1 h WPA2 soak), Phase 8 (robustness on the Pi with a proper power supply), Phase 9
   (packaging, firmware extraction decision).

## Dev-host leftovers at pause time (this project's own setup)

These live outside the repo; remove them if the hosts are repurposed:
- **x86 dev box:**
  - `/lib/firmware/brcm/bcm4322-bmac-5.100.148.5.trx` (dev-only firmware install)
  - the work directory (`DEV_WORKDIR`) and private vendor sequences in `~/bmu-private`
  - scratch files and captures in `/tmp/bmu*` and `/tmp/bmu-*.pcap` (they hold real network data;
    delete any `wpa*.conf`, `wpa*.log`, `dhcpcd.out`, `ping*.out` there)
  - packages `tcpdump` and `dhcpcd` (service not enabled)
  - a runtime-only udev rule in `/run` (gone on reboot)
  - passwordless sudo, if it was enabled for development
- **Raspberry Pi:** the work directory (`TARGET_WORKDIR`), `python3-usb`, and passwordless sudo, if
  it was enabled.
- **Windows capture host:** USBPcap, Wireshark, the staged vendor driver (`pnputil` oem entry) and
  the capture work folder.
- **Build host:** Docker volume `bmu-kbuild` (prepared kernel tree) and image `bmu-kb`.

## License

Driver code derived from brcmsmac keeps its ISC notices (`Dual BSD/GPL` module license); new tools
are ISC. See [docs/legal.md](docs/legal.md). No vendor firmware, vendor driver files or USB
captures are included.
