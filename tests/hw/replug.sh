#!/usr/bin/env bash
# Software "replug" cycles for brcmsmac_usb: toggles the dongle's sysfs `authorized` flag
# (full disconnect + probe) N times, then exercises hang recovery R times via debugfs fault
# injection (which issues a USB port reset). The module must already be loaded.
# With IFUP=1 the mac80211 interface is taken UP after every re-probe, so each de-authorize
# disconnects a running interface.
# usage: [IFUP=1] tests/hw/replug.sh [N] [R]
set -uo pipefail
N=${1:-50}; R=${2:-3}
DEV=$(grep -l '^0846$' /sys/bus/usb/devices/*/idVendor | xargs -n1 dirname | while read -r d; do
	[ "$(cat "$d/idProduct")" = 9020 ] && echo "$d"; done | head -1)
[ -n "$DEV" ] || { echo "dongle not found"; exit 1; }
DBG() { sudo sh -c "cat /sys/kernel/debug/brcmsmac_usb-*/$1" 2>/dev/null; }
IFUP=${IFUP:-0}
ourif() { local n; for n in /sys/class/net/*; do
	[ "$(basename "$(readlink -f "$n/device/driver" 2>/dev/null)")" = brcmsmac_usb ] && basename "$n" && return 0; done; return 1; }
ifup() { local IF=""; for _ in $(seq 1 75); do IF=$(ourif) && break; sleep 0.2; done
	[ -n "$IF" ] || return 1
	nmcli -t -f DEVICE,STATE dev 2>/dev/null | grep -q "^$IF:unmanaged" || sudo nmcli dev set "$IF" managed no
	sudo ip link set "$IF" up && sleep 0.5 && ip link show "$IF" | grep -q ',UP'; }
slab0=$(awk '/^Slab:/{print $2}' /proc/meminfo)
wait_up() { for _ in $(seq 1 75); do DBG state | grep -q '^state: *up' && DBG state | grep -q '^chip: *0x4322' && return 0; sleep 0.2; done; return 1; }
mark="replug-$(date +%s)"; echo "$mark" | sudo tee /dev/kmsg >/dev/null
pass=0; fail=0
for i in $(seq 1 "$N"); do
	echo 0 | sudo tee "$DEV/authorized" >/dev/null; sleep 0.3
	echo 1 | sudo tee "$DEV/authorized" >/dev/null
	if wait_up && { [ "$IFUP" = 0 ] || ifup; }; then pass=$((pass+1)); else
		fail=$((fail+1)); echo "authorize round $i failed: $(DBG state | head -1)"; fi
done
echo "authorize cycles $N pass $pass fail $fail"
rp=0; rf=0
for i in $(seq 1 "$R"); do
	sudo sh -c 'for f in /sys/kernel/debug/brcmsmac_usb-*/fault; do echo hang > "$f"; done'
	sleep 3
	if wait_up; then rp=$((rp+1)); else rf=$((rf+1)); echo "reset round $i failed"; fi
done
echo "fault/reset recoveries $R pass $rp fail $rf"
echo "Slab ${slab0}kB -> $(awk '/^Slab:/{print $2}' /proc/meminfo)kB; sync errors/timeouts now: $(DBG stats | awk '/^sync_(errs|timeouts)/{printf "%s ", $2}')"
echo "rpc failures logged: $(sudo dmesg | sed -n "/$mark/,\$p" | grep -c 'rpc [0-9]* failed')"
echo "kernel log problems: $(sudo dmesg | sed -n "/$mark/,\$p" | grep -cE 'Oops|BUG|WARNING|hung_task|Call Trace')"
sudo dmesg | sed -n "/$mark/,\$p" | grep -E "reset|hung|giving up|budget" | head -8
