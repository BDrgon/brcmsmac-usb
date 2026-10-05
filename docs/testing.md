# Testing

How the hardware gates are exercised. Read the safety rules below before running anything against
a real adapter.

## Safety rules

These rules come from incidents in this project, each of which cost a physical replug.

1. **Never touch the MAC core while it is down.** Register, SHM or TSF calls before `BMAC_INIT` or
   after `DOWN_FINISH` hard-hang the adapter, and a USB port reset does not recover it. The driver
   refuses these calls (`refusing rpc N: d11 core not initialised`). Send raw debugfs calls only
   while the interface is up.
2. **Don't truncate the vendor init sequence mid-block** in `rpcprobe` experiments. Cutting it
   partway through the EDCF parameter writes hung the adapter. Cut only at whole-block boundaries.
3. **Transmit only on 2.4 GHz channels 1–11**, at modest volume. TX is off by default
   (`allow_tx=0`).
4. **Run every association, DHCP lease and ping in a network namespace** (`bcmtest`), so the host's
   own routes, ARP and resolver are never touched. On a dev box whose only uplink is another
   Broadcom card on the `wl` driver, never unload `wl`/`cfg80211` and never load in-tree
   `b43`/`ssb`/`bcma`/`brcmsmac`.
5. **Keep credentials out of the repo.** wpa_supplicant configs live outside the repo, mode 600, and
   are deleted after tests, along with wpa_supplicant debug logs and DHCP/ping outputs.
6. **Watch power on the Pi.** Log `vcgencmd get_throttled` before and after a session. Failures that
   coincide with undervoltage are suspect.

## Module parameters

| Parameter | Default | Purpose |
|---|---|---|
| `allow_tx` | 0 | Let frames reach the radio (runtime-writable) |
| `allow_reset` | 1 | Attempt a USB port reset when the adapter stops answering |
| `identify` | 1 | Read and log revinfo/NVRAM/MAC after connecting |
| `fw_version` | 5.100.148.5 | Firmware version to load (must be in the driver's table) |
| `attach_mac80211` | 1 | 0 = transport + debugfs only |
| `reg_hint` | 0 | Pass the adapter's country code to cfg80211 |

## debugfs (root only)

- `/sys/kernel/debug/brcmsmac_usb-<intf>/`
  - `state`: identity, session, BMAC_INIT/guard state
  - `stats`: sync/async counters, errors, timeouts
  - `trace`: ring buffer of the last 2048 RPC messages
  - `rpc`: write `sync <id> <args…>` / `call <id> <args…>`; read back the reply
  - `fault`: inject a simulated hang (exercises recovery)
- `brcmsmac_usb_wl/<dev>/`
  - `last_tx`: last TX buffer
  - `txlog`: recent tx statuses
  - `rxtx`: RX/TX counters

## Hardware test scripts (`tests/hw/`, run on the dev host from the repo root)

| Script | Gate | What it does |
|---|---|---|
| `reload.sh [N] [KO]` | G3 | N load/unload cycles; each must reach state `up` with the expected identity |
| `replug.sh [N] [R]` (`IFUP=1` optional) | G3/G4 | N software replugs (sysfs `authorized`), then R fault-injected recoveries; optionally with the interface up |
| `cycle.sh [N] [KO] [params]` | G4 | load → up/down/up → RPC counter check → unload while up |
| `monitor.sh [CHAN] [SECONDS]` (`REF=`, `MONFLAGS=`) | G5 | monitor capture + `tools/pcapsum.py` summary, compared with another receiver's cached scan |
| `netns-assoc.sh [PING_SECONDS] [ATTEMPTS]` (`WPA_CONF=`) | G6/G7 | association, DHCP and ping inside netns `bcmtest`; always cleans up |

Unit tests: `pytest tests` (no hardware; uses sanitised fixtures).

Helpers:
- `tools/pcapsum.py` summarises monitor captures.
- `tools/dev/txcmp.py` compares `last_tx` with a vendor TX frame.
- `tools/usbrecover.py` tries a software recovery (`reset|authorize|status`). It does not clear a
  hard hang.

## Capturing the vendor driver (Windows, USBPcap)

Protocol facts come from captures of the vendor driver talking to the adapter.

1. On an x86 Windows machine, install USBPcap and Wireshark. If USBPcap's silent installer leaves the
   filter driver uninstalled, install it from its INF and restart the USB root hubs.
2. Install the vendor driver (`bcmwlhigh6.inf`, Device Manager → Have Disk).
3. Start the capture **before** plugging in the adapter, with
   `USBPcapCMD -d \\.\USBPcapN -o out.pcap -A --capture-from-new-devices --inject-descriptors`.
   Without `--capture-from-new-devices`, a device plugged in after the start is not recorded at all.
4. Plug in, then step through idle → scan → WPA2 associate → DHCP → ping → disconnect → unplug,
   writing down a timestamp at each step.
5. Decode with `tools/rpcdecode.py <pcap> <devaddr> --dump`. Keep raw captures private (they hold
   real MACs and network names). Commit only sanitised fixtures (`tools/mkfixtures.py`).
