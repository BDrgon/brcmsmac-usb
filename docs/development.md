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

Firmware is never part of this repository; see SPEC §6.
