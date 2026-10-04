#!/usr/bin/env bash
# Build an out-of-tree module.
#
# usage: scripts/build.sh [pi|native|x86] [MODULE_DIR]     (default: pi driver/hello)
#   pi      cross-build on this machine in Docker (linux/arm/v7 container, emulated on non-ARM32
#           hosts) against the target's own kernel header packages, fetched once into
#           work/kheaders/<release>/ (gitignored)
#   native  copy the source to the target and build there (slow on a Pi 1, needs no Docker)
#   x86     build in a linux/amd64 Debian container against its stock kernel headers (compile check)
# Output: build/<mode>/<module>.ko
set -euo pipefail
. "$(dirname "$0")/common.sh"
MODE="${1:-pi}"
MOD="${2:-driver/hello}"
MODNAME="$(basename "$MOD")"
OUT="$ROOT/build/$MODE"
mkdir -p "$OUT"

fetch_headers() {	# copy the target's header + kbuild packages, preserving absolute paths
	local krel="$1"
	local dst="$ROOT/work/kheaders/$krel"
	[ -f "$dst/.complete" ] && { echo "$dst"; return; }
	mkdir -p "$dst"
	local base="${krel%-rpi-*}"		# 6.18.50+rpt
	tssh "tar -C / -cf - usr/src/linux-headers-$krel usr/src/linux-headers-$base-common-rpi \
		usr/lib/linux-kbuild-$base" | tar -C "$dst" -xf -
	touch "$dst/.complete"
	echo "$dst"
}

case "$MODE" in
pi)
	command -v docker >/dev/null || { echo "docker not found" >&2; exit 1; }
	docker info >/dev/null 2>&1 || { echo "Docker daemon not running: start Docker Desktop, or use: $0 native $MOD" >&2; exit 2; }
	KREL="$(target_krel)"
	HDR="$(fetch_headers "$KREL")"
	docker run --rm --platform linux/arm/v7 \
		-v "$HDR/usr/src:/usr/src:ro" -v "$HDR/usr/lib/linux-kbuild-${KREL%-rpi-*}:/usr/lib/linux-kbuild-${KREL%-rpi-*}:ro" \
		-v "$ROOT:/src" -w /src debian:trixie bash -c "
		set -e
		apt-get -qq update >/dev/null && apt-get -qq install -y gcc make libc6-dev bc >/dev/null
		rm -rf /tmp/m && cp -r /src/$MOD /tmp/m
		make -s -C /usr/src/linux-headers-$KREL M=/tmp/m modules
		cp /tmp/m/*.ko /src/build/pi/"
	;;
native)
	KREL="$(target_krel)"
	tssh "mkdir -p '$TARGET_WORKDIR/src'"
	tar -C "$ROOT/$(dirname "$MOD")" -cf - "$MODNAME" | tssh "rm -rf '$TARGET_WORKDIR/src/$MODNAME' && tar -C '$TARGET_WORKDIR/src' -xf -"
	tssh "make -s -C /lib/modules/$KREL/build M='$TARGET_WORKDIR/src/$MODNAME' modules"
	tssh "cat '$TARGET_WORKDIR'/src/'$MODNAME'/*.ko" > /dev/null	# existence check
	for ko in $(tssh "cd '$TARGET_WORKDIR/src/$MODNAME' && ls *.ko"); do
		tssh "cat '$TARGET_WORKDIR/src/$MODNAME/$ko'" > "$OUT/$ko"
	done
	;;
x86)
	command -v docker >/dev/null && docker info >/dev/null 2>&1 || { echo "Docker daemon not running" >&2; exit 2; }
	docker run --rm --platform linux/amd64 -v "$ROOT:/src" -w /src debian:trixie bash -c "
		set -e
		apt-get -qq update >/dev/null && apt-get -qq install -y gcc make bc linux-headers-amd64 >/dev/null
		K=\$(ls -d /usr/src/linux-headers-*-amd64 | head -1)
		rm -rf /tmp/m && cp -r /src/$MOD /tmp/m
		make -s -C \$K M=/tmp/m modules
		cp /tmp/m/*.ko /src/build/x86/"
	;;
*) echo "usage: $0 [pi|native|x86] [MODULE_DIR]" >&2; exit 1 ;;
esac
ls -la "$OUT"
