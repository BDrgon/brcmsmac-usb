# brcmsmac-usb — SPEC

A Linux kernel driver for Broadcom **BCM4322 USB "BMAC" (split-MAC) dongles**, first target the
**Netgear WNA3100 v1** (`0846:9020`), built by adapting the in-tree `brcmsmac` driver to drive the
dongle's LOW-MAC firmware over Broadcom's USB RPC protocol.

This document is the fixed target. How we get there — order of work, gates, decisions, risks —
lives in [PLAN.md](PLAN.md). Protocol facts live in `docs/protocol.md` (promoted from
`work/PROTOCOL.md`).

---

## 1. Problem

These dongles have never worked on Linux. The USB BCM4322 is a split-MAC design:

| Half | Runs on | Contains |
|---|---|---|
| LOW (`wlc_bmac`) | dongle ARM, firmware downloaded at plug-in | d11 MAC core, PHY/radio, calibration, DMA, crypto engine |
| HIGH (`wlc`) | host | 802.11 MAC: scan, MLME, rate control, AMPDU, key management, mac80211 glue |

The two halves talk over a proprietary RPC protocol on USB bulk/control endpoints.

- `brcmfmac` (fullmac) and `rndis_wlan` don't fit: the firmware has no 802.11 stack to talk to.
- `b43`/`brcmsmac` (softmac PCI) assume direct register/DMA access.
- Broadcom's own HIGH-side RPC layer (`WLC_HIGH_ONLY`) was removed from Linux staging in 2010 before
  it was ever complete.

## 2. Deliverable

An **out-of-tree Linux kernel module**, `brcmsmac_usb.ko`, that:

1. Binds `USB 0846:9020` (and other IDs added only once proven).
2. Loads user-provided firmware through `request_firmware()`, downloads it via the boot-ROM RDL
   protocol, starts it, and opens an RPC session with an exact firmware-version handshake.
3. Registers with **mac80211** as a softmac `ieee80211_hw`, reusing `brcmsmac`'s upper MAC.
   Where `brcmsmac` touches hardware directly (registers, DMA, PHY, bcma), it issues RPCs instead.
4. Works with standard userspace: `iw`, `wpa_supplicant`, NetworkManager / systemd-networkd.

The module ships with:
- a **firmware extraction tool**, `tools/fwextract`, run by the user on their own copy of the
  vendor Windows driver package;
- **userspace protocol tools** (`tools/`): RDL loader, RPC probe, capture decoder;
- **protocol documentation** (`docs/protocol.md`);
- **DKMS packaging**.

## 3. Functional requirements

| ID | Requirement | Acceptance |
|---|---|---|
| F1 | Probe + firmware download + RPC session on hot-plug and at boot | `dmesg` shows chip, revinfo, MAC; interface `wlanN` appears |
| F2 | Station mode, 2.4 GHz, 802.11b/g/n 20 MHz | `iw dev wlanN scan` lists nearby APs with correct SSID, channel and signal |
| F3 | WPA2-PSK (CCMP) via `wpa_supplicant` | Associates, completes the 4-way handshake, gets DHCP, passes traffic |
| F4 | Open networks | Associates and passes traffic |
| F5 | Monitor mode (RX) | `tcpdump -i monN` shows radiotap + 802.11 frames |
| F6 | Clean unload / surprise removal | `rmmod`, unplug mid-traffic, and replug cause no oops, leak or hung task |
| F7 | Firmware hang recovery | RPC timeout leads to a USB reset and re-init without a reboot |
| F8 | Correct regulatory behaviour | Honours the cfg80211 regdomain; no TX outside allowed channels |

**Stretch goals**, not required for v1: 5 GHz (the radio is a/b/g/n), HT40, AMPDU aggregation,
AP mode via hostapd, other BCM4322/43231 USB IDs, the 5.60 firmware.

## 4. Non-functional requirements

| ID | Requirement |
|---|---|
| N1 | Target kernel: Raspberry Pi OS `6.18.x` (armv6 `rpi-v6`, Pi 1 B+) first. Must also build for x86_64 6.x so non-Pi users can test. |
| N2 | Throughput is not a goal. Stability is: a 24 h ping soak with no disconnects, plus an `iperf3` baseline recorded. |
| N3 | Low-power friendly: no busy-polling; URB-driven RX. |
| N4 | Kernel coding style (`checkpatch.pl` clean on new code). Sparse-clean. |
| N5 | Debuggability: debugfs for raw RPC calls, counters and a last-N RPC trace; dynamic debug. |

## 5. Architecture

```
            mac80211 / cfg80211
                   │
   ┌───────────────┴────────────────┐
   │ brcmsmac upper MAC (forked)     │  mac80211_if.c, main.c (HIGH part), ampdu.c,
   │                                 │  antsel.c, channel.c, rate.c, stf.c, pmu-free
   └───────────────┬────────────────┘
                   │  brcms_b_* / wlc_phy_* / register + FIFO ops
   ┌───────────────┴────────────────┐
   │ bmac_rpc.c — RPC stubs          │  one stub per used WLRPC id; XDR marshalling
   └───────────────┬────────────────┘
   ┌───────────────┴────────────────┐
   │ rpc.c — sessions & calls        │  HELLO/CONNECT, xid, sync RTN calls,
   │                                 │  async dispatch (WLC_RECV, DOTXSTATUS, …)
   └───────────────┬────────────────┘
   ┌───────────────┴────────────────┐
   │ usb.c — transport & RDL         │  probe/disconnect, firmware download,
   │                                 │  bulk OUT (aggregation), bulk IN URBs, ctrl req 9 returns
   └────────────────────────────────┘
```

Removed from the fork (they run on the dongle): `phy/`, `dma.c`, `aiutils.c`/bcma glue, PMU,
direct `bcma_*` register access.

Per-firmware data (RPC ID table, version word, TRX hash) lives in a **firmware table** inside the
driver, keyed by the version the firmware reports. Supporting another firmware version means adding
a table entry, not changing code.

## 6. Firmware & licensing policy

This is the line we don't cross. Shared artifacts must be redistributable.

| Item | Status | Policy |
|---|---|---|
| Dongle firmware (`rtecdc` TRX inside `bcmwlhigh6.sys`) | Broadcom proprietary | **Never shipped.** Users run `tools/fwextract` on their own vendor package. The tool verifies the result by SHA-256 against a table of known-good hashes. |
| Vendor Windows driver (Netgear `WNA3100-SW-*.zip`) | Proprietary | **Never shipped.** The README tells users where to get it. |
| `brcmsmac` (mainline) | ISC | Fork and modify freely; keep the notices. |
| Linux v2.6.37 staging `brcm80211` (`wlc_rpc.h` etc.) | ISC | May be used as reference and source for ID enums. |
| Router-tree `bcm_rpc.c` / `bcm_xdr.c` (asuswrt/Tomato) | **"UNPUBLISHED PROPRIETARY SOURCE CODE" — not GPL** | **Clean-room only.** No code, structure or comments copied. Protocol facts come from USB captures of hardware we own plus the ISC headers. Anyone who has read these files must not write the corresponding driver code without first producing an independent spec in `docs/protocol.md` from capture evidence. These files stay in the private `work/` area and are never committed. |
| USB captures | Our own traffic, but they contain network names, MACs and possibly keys | Never shipped raw. Sanitised excerpts only, in docs and tests. |

**Module license:** `Dual ISC/GPL`, matching brcmsmac (`MODULE_LICENSE("Dual BSD/GPL")`).
New tools are ISC.

## 7. Configuration (not shipped)

Anything specific to one installation lives in `config/local.env`, which is gitignored. A template
is provided as `config/local.env.example`. This covers hostnames, IPs, SSH targets, test SSIDs and
passphrases, and the paths to the user's vendor package. Scripts read it; code and docs never
hard-code it.

## 8. Repository layout

```
SPEC.md  PLAN.md  README.md  LICENSE
config/local.env.example        # template; local.env is gitignored
docs/protocol.md                # wire protocol (independent spec, capture-derived)
docs/decisions.md               # ADR log (mirrors PLAN decisions once settled)
driver/                         # kernel module source + Kbuild + dkms.conf
tools/                          # fwextract, rdl, rpcprobe (userspace protocol lib), capture decoders
tests/                          # test scripts + sanitised fixtures
work/                           # PRIVATE scratch: firmware, captures, vendor files, proprietary refs (gitignored)
```

## 9. Definition of done (v1)

On a Raspberry Pi 1 B+ running Raspberry Pi OS (6.18, armv6), with only the DKMS package and
user-extracted firmware installed:

1. Plug in a WNA3100 v1. `wlan1` appears within 10 s.
2. `wpa_supplicant` joins a WPA2-PSK 2.4 GHz network, then DHCP succeeds.
3. A 24 h ping soak to the gateway shows < 0.1 % loss and no driver resets. An `iperf3` result is recorded.
4. Unplug/replug and `rmmod`/`modprobe`, ×50 each, cause no oops, leak (kmemleak) or hung task.
5. The same source builds and passes items 1–2 on an x86_64 6.x kernel.
6. A fresh user can go from vendor zip to working Wi-Fi with only the README.
