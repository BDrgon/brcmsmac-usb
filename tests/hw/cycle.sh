#!/usr/bin/env bash
# Load/unload cycles with the mac80211 interface in use: each round loads brcmsmac_usb, waits for
# the network interface, takes it up/down/up, checks the RPC counters and unloads it while the
# interface is UP. Run on the dev box from the repo root (needs sudo).
# usage: tests/hw/cycle.sh [N] [KO] [insmod params...]
set -uo pipefail
N=${1:-20}
KO=${2:-driver/brcmsmac_usb.ko}
shift 2 2>/dev/null || shift $#
DBG() { sudo sh -c "cat /sys/kernel/debug/brcmsmac_usb-*/$1" 2>/dev/null; }
ourif() {
	local n
	for n in /sys/class/net/*; do
		[ "$(basename "$(readlink -f "$n/device/driver" 2>/dev/null)")" = brcmsmac_usb ] &&
			basename "$n" && return 0
	done
	return 1
}
klog() { sudo dmesg | sed -n "/$mark/,\$p" | grep -v nouveau; }
mark="cycle-$(date +%s)"; echo "$mark" | sudo tee /dev/kmsg >/dev/null
lsmod | grep -q '^brcmsmac_usb ' && sudo rmmod brcmsmac_usb
sleep 1
slab0=$(awk '/^Slab:/{print $2}' /proc/meminfo)
k0=$(sudo awk '/^kmalloc-/{s+=$2} END{print s}' /proc/slabinfo)
pass=0; fail=0
for i in $(seq 1 "$N"); do
	sudo insmod "$KO" "$@"
	IF=""
	for _ in $(seq 1 75); do IF=$(ourif) && break; sleep 0.2; done
	if [ -z "$IF" ]; then fail=$((fail+1)); echo "round $i: no interface ($(DBG state | head -1))"; sudo rmmod brcmsmac_usb; continue; fi
	nmcli -t -f DEVICE,STATE dev 2>/dev/null | grep -q "^$IF:unmanaged" || sudo nmcli dev set "$IF" managed no
	ok=1
	sudo ip link set "$IF" up || ok=0
	sleep 1
	sudo ip link set "$IF" down || ok=0
	sudo ip link set "$IF" up || ok=0
	sleep 1
	st=$(DBG stats)
	to=$(awk '/^sync_timeouts/{print $2}' <<<"$st"); se=$(awk '/^sync_errs/{print $2}' <<<"$st")
	te=$(awk '/^tx_errs/{print $2}' <<<"$st"); rx=$(awk '/^async_id_81 /{print $2}' <<<"$st")
	[ "${to:-1}" = 0 ] && [ "${se:-1}" = 0 ] && [ "${te:-1}" = 0 ] || ok=0
	ip link show "$IF" | grep -q ',UP' || ok=0
	sudo rmmod brcmsmac_usb || ok=0
	if [ $ok = 1 ]; then pass=$((pass+1)); else fail=$((fail+1)); fi
	echo "round $i: if=$IF sync_timeouts=$to sync_errs=$se tx_errs=$te rx_frames=${rx:-0} ok=$ok"
done
sleep 1
slab1=$(awk '/^Slab:/{print $2}' /proc/meminfo)
k1=$(sudo awk '/^kmalloc-/{s+=$2} END{print s}' /proc/slabinfo)
echo "rounds $N pass $pass fail $fail; Slab ${slab0}kB -> ${slab1}kB; kmalloc objects $k0 -> $k1"
echo "rpc failures logged: $(klog | grep -c 'rpc [0-9]* failed')"
klog | grep -E "rpc [0-9]* failed" | sort | uniq -c | head
klog | grep -E "Oops|BUG|WARNING|hung_task|refcount|list_del|use-after|general protection|Call Trace" | head -20
echo "kernel log problems: $(klog | grep -cE 'Oops|BUG|WARNING|hung_task|Call Trace')"
