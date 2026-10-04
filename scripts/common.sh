# shellcheck shell=bash
# Shared helpers: repo root and config/local.env (KEY=value lines; never committed).
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if [ -f "$ROOT/config/local.env" ]; then
	set -a
	# shellcheck disable=SC1091
	. "$ROOT/config/local.env"
	set +a
fi
: "${TARGET_SSH:?set TARGET_SSH in config/local.env (see config/local.env.example)}"
: "${TARGET_WORKDIR:?set TARGET_WORKDIR in config/local.env}"
tssh() { ssh -o BatchMode=yes "$TARGET_SSH" "$@"; }
target_krel() { if [ -n "${TARGET_KERNEL:-}" ]; then echo "$TARGET_KERNEL"; else tssh uname -r; fi; }
