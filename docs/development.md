# Development loop

## Setup

1. Copy `config/local.env.example` to `config/local.env` and fill it in (target SSH alias, work
   directory on the target). Never commit `local.env`.
2. Target needs: kernel headers for its running kernel (Raspberry Pi OS: `linux-headers-rpi-v6`,
   usually installed), `python3-usb` for the userspace tools, and passwordless sudo during development.
3. Build host needs Docker for cross-builds. Without Docker, use `native` builds on the target.

## Build

```
scripts/build.sh pi     [MODULE_DIR]   # Docker linux/arm/v7 container against the target's own header
                                       # packages (cached in work/kheaders/<release>/); ~1.5 min first time
scripts/build.sh native [MODULE_DIR]   # build on the target itself (~1 min for a trivial module on a Pi 1)
scripts/build.sh x86    [MODULE_DIR]   # compile check against Debian's amd64 headers
```

Output: `build/<mode>/*.ko` (gitignored). `MODULE_DIR` defaults to `driver/hello` (toolchain smoke test).

## x86 dev box (kernel module work)

`config/local.env` sets `DEV_SSH` and `DEV_WORKDIR`. The dev box builds natively against its own
running kernel (`/lib/modules/$(uname -r)/build`):

```
scripts/dev.sh build             # rsync repo (no work/, no local.env) + make in driver/
scripts/dev.sh load [PARAMS]     # build, reload brcmsmac_usb, print new kernel log lines
scripts/dev.sh unload
scripts/dev.sh ssh CMD           # run a command there
```

The driver needs the firmware at `/lib/firmware/brcm/bcm4322-bmac-5.100.148.5.trx` (copy your own
extracted image there; dev-only install).

debugfs (`/sys/kernel/debug/brcmsmac_usb-<intf>/`, root only):

| File | Use |
|---|---|
| `state` | session state, firmware, chip, MAC, NVRAM head, whether BMAC_INIT was sent |
| `stats` | message/byte/error counters, async messages per rpc id |
| `trace` | last 256 messages (`O` out, `I` bulk in, `R` sync reply) with the first 32 bytes |
| `rpc` | `echo "sync 76" > rpc; cat rpc` — raw calls (`sync|call ID [u32 ARGS...]`) |
| `fault` | `echo hang > fault` — simulate a hung dongle (exercises reset recovery) |

Hardware tests (run on the dev box from `DEV_WORKDIR`): `tests/hw/reload.sh [N]`,
`tests/hw/replug.sh [N] [R]` (software replug via sysfs `authorized`, then R reset recoveries).

## Deploy

```
scripts/deploy.sh [pi|native] [MODULE.ko]          # copy, rmmod old copy, insmod, show new dmesg lines
scripts/deploy.sh [pi|native] "" --unload          # unload only
```

Idempotent. Prints `vcgencmd get_throttled` before and after, since a Pi 1 PSU can be marginal.
Modules are only `insmod`ed from the work directory; nothing is installed system-wide.

## Userspace tools on the target

Copy `tools/` to the target (`scp -r tools TARGET:WORKDIR/`), then:

```
sudo python3 tools/rdl.py info
sudo python3 tools/rpcprobe/m2.py --fw <firmware.trx>
sudo python3 tools/usbrecover.py status
```

### rpcprobe (Phase 2 reference driver)

Run from the `tools/` directory on the target (`python3 -m rpcprobe --help`):

```
sudo python3 -m rpcprobe info                                  # boot ROM or firmware running?
sudo python3 -m rpcprobe --fw FW.trx --trace T.jsonl probe     # download + identity calls
sudo python3 -m rpcprobe --trace T.jsonl up --seq SEQ.jsonl    # replay init through UP_FINISH
sudo python3 -m rpcprobe --trace T.jsonl rx --no-up --seq SEQ.jsonl \
     --chanblocks BLK.json --chan 6 --chan 1 --seconds 10     # retune + list beacons (receive only)
sudo python3 -m rpcprobe --trace T.jsonl tx --seq SEQ.jsonl --chanblocks BLK.json --chan 6 \
     --ssid-hex HEX --bcast 2 --directed 2 --out T.json        # transmit a few probe requests
```

`tx` brings the dongle up (unless `--no-up`), tunes, listens `--listen` seconds, then sends `--bcast`
wildcard broadcast probes and `--directed` unicast probes (CCK 1 Mbit/s, FIFO 3) to the strongest AP
on the channel, or to the one whose SSID matches `--ssid-hex` (hex avoids shell quoting). It prints
each frame's tx status (ACK, retries, suppression) and every management frame addressed to our MAC.
Channels are limited to 1–11 and frame counts to 5 each; it isn't a flood tool.

**Power:** a Pi 1 on a weak supply browns out when the radio is active. Before and after hardware
sessions, record `vcgencmd get_throttled` and `dmesg | grep -ci undervoltage`, and don't trust a
failure that coincides with an undervoltage event.

`SEQ.jsonl` / `BLK.json` come from your own capture of the vendor driver (see `docs/protocol.md`):

```
python3 tools/seqextract.py CAPTURE.pcap DEV -o work/seq/up.jsonl            # init sequence
python3 tools/seqextract.py CAPTURE.pcap DEV --chanblocks work/seq/blk.json  # retune blocks
```

These files, and traces, contain device data (MACs, SSIDs, NVRAM): keep them in `work/`.
Unit tests (no hardware): `python3 -m unittest discover -s tests -v`.

Firmware is never part of this repository; see SPEC §6.
