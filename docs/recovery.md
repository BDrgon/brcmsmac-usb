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
