#!/usr/bin/env bash
# x86 development loop on DEV_SSH (native build on the dev box).
#
# usage: scripts/dev.sh sync              # rsync repo (no work/, no config/local.env) to DEV_WORKDIR
#        scripts/dev.sh build             # sync + make in driver/ on the dev box
#        scripts/dev.sh load [PARAMS...]  # build, rmmod old copy, insmod, show new dmesg lines
#        scripts/dev.sh unload
#        scripts/dev.sh ssh CMD...        # run a command on the dev box
# Never touches modules other than brcmsmac_usb.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
set -a; . "$ROOT/config/local.env"; set +a
: "${DEV_SSH:?set DEV_SSH in config/local.env}"
: "${DEV_WORKDIR:?set DEV_WORKDIR in config/local.env}"
dssh() { ssh -o BatchMode=yes "$DEV_SSH" "$@"; }
MOD=brcmsmac_usb

sync_repo() {
	rsync -a --delete --exclude work/ --exclude config/local.env --exclude .git/ \
		--exclude build/ --exclude '__pycache__/' "$ROOT/" "$DEV_SSH:$DEV_WORKDIR/"
}
build() { sync_repo; dssh "make -C '$DEV_WORKDIR/driver' -j2 2>&1 | grep -E 'warning|error|Error' || true; ls -la '$DEV_WORKDIR/driver/$MOD.ko'"; }
mark() { local m="dev-$MOD-$(date +%s)"; dssh "echo '$m' | sudo tee /dev/kmsg >/dev/null"; echo "$m"; }
newlog() { dssh "sudo dmesg | sed -n '/$1/,\$p' | tail -n +2"; }

case "${1:-}" in
sync) sync_repo ;;
build) build ;;
load)
	shift; build; M=$(mark)
	dssh "lsmod | grep -q '^$MOD ' && sudo rmmod $MOD; sudo insmod '$DEV_WORKDIR/driver/$MOD.ko' $*"
	sleep "${WAIT:-4}"; newlog "$M" ;;
unload) M=$(mark); dssh "lsmod | grep -q '^$MOD ' && sudo rmmod $MOD || true"; newlog "$M" ;;
ssh) shift; dssh "$@" ;;
*) sed -n '2,9p' "$0"; exit 1 ;;
esac
