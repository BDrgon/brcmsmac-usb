#!/usr/bin/env bash
# Gate G3: load/unload brcmsmac_usb N times on the dev box; each load must reach state "up"
# with the expected identity. Run on the target from the repo root (needs sudo).
# usage: tests/hw/reload.sh [N] [KO]
set -uo pipefail
N=${1:-50}
KO=${2:-driver/brcmsmac_usb.ko}
DBG() { sudo sh -c "cat /sys/kernel/debug/brcmsmac_usb-*/$1" 2>/dev/null; }
mark="reload-$(date +%s)"; echo "$mark" | sudo tee /dev/kmsg >/dev/null
slab0=$(awk '/^Slab:/{print $2}' /proc/meminfo)
pass=0; fail=0
for i in $(seq 1 "$N"); do
	lsmod | grep -q '^brcmsmac_usb ' && sudo rmmod brcmsmac_usb
	sudo insmod "$KO"
	st=""
	for _ in $(seq 1 50); do
		st=$(DBG state | awk '/^state:/{print $2}')
		[ "$st" = up ] && DBG state | grep -q '^chip: *0x4322' && break
		sleep 0.2
	done
	if [ "$st" = up ] && DBG state | grep -q '^chip: *0x4322'; then pass=$((pass+1)); else
		fail=$((fail+1)); echo "round $i: state='$st'"; fi
done
sudo rmmod brcmsmac_usb
slab1=$(awk '/^Slab:/{print $2}' /proc/meminfo)
echo "rounds $N pass $pass fail $fail; slab ${slab0}kB -> ${slab1}kB"
sudo dmesg | sed -n "/$mark/,\$p" | grep -E "Oops|BUG|WARNING|hung_task|refcount|list_del|use-after|general protection|Call Trace" | head -20 || true
echo "kernel log problems: $(sudo dmesg | sed -n "/$mark/,\$p" | grep -cE 'Oops|BUG|WARNING|hung_task|Call Trace')"
echo "session lines: $(sudo dmesg | sed -n "/$mark/,\$p" | grep -c 'RPC session up')  reconnects: $(sudo dmesg | sed -n "/$mark/,\$p" | grep -c 'reconnected')"
