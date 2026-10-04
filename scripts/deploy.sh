#!/usr/bin/env bash
# Copy a built module to the target, (re)load it and show the new kernel log lines.
#
# usage: scripts/deploy.sh [MODE] [MODULE.ko] [--unload]
#   MODE      build output to deploy from (pi|native), default: pi if build/pi has a .ko, else native
#   --unload  only unload it (and show dmesg)
# Idempotent: unloads a previously loaded copy first. Reports `vcgencmd get_throttled` before/after.
set -euo pipefail
. "$(dirname "$0")/common.sh"
MODE="${1:-}"
if [ -z "$MODE" ]; then ls "$ROOT"/build/pi/*.ko >/dev/null 2>&1 && MODE=pi || MODE=native; fi
KO="${2:-$(ls "$ROOT/build/$MODE/"*.ko | head -1)}"
NAME="$(basename "$KO" .ko)"
UNLOAD_ONLY=0; [[ " $* " == *" --unload "* ]] && UNLOAD_ONLY=1

tssh "mkdir -p '$TARGET_WORKDIR/modules'"
echo "power before: $(tssh 'sudo vcgencmd get_throttled' 2>/dev/null || echo n/a)"
MARK="deploy-$NAME-$(date +%s)"
tssh "echo '$MARK' | sudo tee /dev/kmsg >/dev/null"
if tssh "lsmod | grep -q '^${NAME} '"; then tssh "sudo rmmod '$NAME'"; fi
if [ "$UNLOAD_ONLY" = 0 ]; then
	tssh "cat > '$TARGET_WORKDIR/modules/$NAME.ko'" < "$KO"
	tssh "sudo insmod '$TARGET_WORKDIR/modules/$NAME.ko'"
	tssh "lsmod | grep '^${NAME} '" || { echo "module not loaded" >&2; exit 1; }
fi
tssh "sudo dmesg | sed -n '/$MARK/,\$p' | tail -n +2"
echo "power after:  $(tssh 'sudo vcgencmd get_throttled' 2>/dev/null || echo n/a)"
