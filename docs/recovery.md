# Dongle recovery without unplugging

Findings from a Raspberry Pi 1 B+ (LAN9514 hub, kernel 6.18.50+rpt-rpi-v6), 2026-10-04.
Tool: `tools/usbrecover.py [status|reset|authorize]`.

## Failure modes seen

| Mode | Trigger | What the host sees |
|---|---|---|
| **Firmware self-reboot** | intermittent; once during read-only sync calls (75/76/77) that pass on other runs | I/O error mid-call, `USB disconnect`, then the device re-enumerates **by itself** about 3 s later as the boot ROM ("Remote Download Wireless Adapter"). No intervention needed; the host must re-download the firmware. |
| **Hard hang** | register access before `BMAC_INIT` (id 1 REG_READ at 0x688) | No reply; vendor request 5 (`DL_GETVER`) times out too. The device stays on the bus but is unresponsive. |

## Recovery attempts on a hard hang

| Method | Result |
|---|---|
| USB port reset (`USBDEVFS_RESET`) | ✗ Re-enumeration fails with `device descriptor read/64, error -32`, then `unable to enumerate USB device`; the device disappears from the bus |
| sysfs `authorized` 0→1 | ✗ Not possible: after the failed reset the device no longer exists |
| Per-port power off/on via sysfs `…/1-1:1.0/1-1-port3/disable` | ✗ The kernel re-runs enumeration, which fails the same way, so **VBUS is not actually switched** |
| Physical replug | ✓ Device returns in the boot ROM |

The LAN9514 descriptor claims per-port power switching (`wHubCharacteristic 0x000d`), but on the
Pi B+ the external ports aren't actually power-switched per port, so a powered dongle stays hung.
Toggling port 3 didn't affect Ethernet (port 1, `smsc95xx`).

## Consequences for the driver (PLAN phases 3 and 8)

- **Self-reboot** looks like a normal disconnect/connect. The driver must handle surprise removal
  cleanly and re-download the firmware when the boot ROM reappears.
- **Hard hang**: the driver can detect it (RPC timeout followed by a `DL_GETVER` timeout) but can't
  fix it in software on this hardware. It should report the hang clearly, stop issuing I/O, and let
  the user replug. **Never issue register access before `BMAC_INIT`.**
- For unattended setups: a powered USB hub with **real** per-port power switching (see the `uhubctl`
  compatibility list) would allow `uhubctl`-based power cycling. Untested here.

## x86 dev box (EHCI, MacBookPro5,5, linux-lts 6.18.53), 2026-10-04

Tests with the `brcmsmac_usb` kernel module (`tests/hw/reload.sh`, `tests/hw/replug.sh`):

| Test | Result |
|---|---|
| 50× `rmmod`/`insmod` | 50/50 reach state `up` with the right identity; every load after the first **reconnects** to the still-running firmware (ACCEPT status 2). No oops/WARN/hung task; slab unchanged |
| 50× sysfs `authorized` 0→1 (software replug: full disconnect + probe) | 50/50 up again; firmware keeps running across it |
| 3× simulated hang (`debugfs fault`) → `usb_queue_reset_device()` | 3/3 recovered: the port reset re-probes the device, which comes back with **firmware still running**, so recovery is a reconnect (≈ 0.3 s), not a re-download |

So on this host a USB port reset does **not** return the dongle to the boot ROM, and does not
power-cycle it. Whether a port reset clears a *hard* hang (where even `DL_GETVER` stops answering)
is still untested here: the only known trigger is register access before `BMAC_INIT`, which the
driver now refuses, and a failed test would need a physical replug.

### Driver behaviour (phase 3)

- A sync call that gets no reply checks `DL_GETVER`; if that also fails the device is marked
  **hung**, I/O stops, and a port reset is queued (`allow_reset=1`, at most 3 per 10 minutes,
  module-wide, because each reset re-probes the device).
- Register/shared-memory RPCs (ids 1, 2, 39, 40, 44, 45, 51, 52) are refused with `-EPERM`
  until `BMAC_INIT` (18) has been sent.

### Hard hang on x86 (2026-10-04)

A synthetic rpcprobe experiment hung the dongle hard: the vendor init sequence replayed only up to
entry 210 (UP_PREP and BMAC_INIT done, stopped in the middle of the EDCF parameter writes, before
UP_FINISH), then the driver's own recorded sequence from its BMAC_INIT onward. A sync call got no
reply and `DL_GETVER` timed out. `USBDEVFS_RESET` (`tools/usbrecover.py reset`) then failed exactly as on
the Pi: `device descriptor read/8, error -32`, `unable to enumerate USB device`, and the device left
the bus. So **on this host too, a port reset does not clear a hard hang; a physical replug is needed.**
The driver itself never produces this order of calls. Lesson for bisecting: cut replayed sequences
only at block boundaries (after UP_FINISH, or between whole SHM blocks).
