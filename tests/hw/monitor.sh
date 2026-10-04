#!/usr/bin/env bash
# Gate G5 (monitor part): monitor interface on a fixed channel, capture with tcpdump, summarise
# with tools/pcapsum.py (radiotap channel/signal, frame types, TSFT, DS IE agreement), compared
# with another receiver's cached scan results (`iw dev $REF scan dump`; read-only, no scan).
# The module must be loaded. Captures stay in /tmp on the target (they hold real MACs/SSIDs).
# MONFLAGS: iw monitor flags, e.g. "fcsfail control otherbss" (default: mac80211's).
# usage: [REF=wlp3s0] [MONFLAGS=...] tests/hw/monitor.sh [CHAN] [SECONDS]
set -uo pipefail
CH=${1:-6}; SECS=${2:-20}; REF=${REF:-}
MON=bmon0; CAP=/tmp/bmu-mon-ch$CH.pcap
case "$CH" in [1-9]|1[01]) ;; *) echo "channel 1-11 only"; exit 1 ;; esac
ourif() { local n; for n in /sys/class/net/*; do
	[ "$(basename "$(readlink -f "$n/device/driver" 2>/dev/null)")" = brcmsmac_usb ] &&
		[ "$(cat "$n/type")" = 1 ] && basename "$n" && return 0; done; return 1; }
DBG() { sudo sh -c "cat /sys/kernel/debug/brcmsmac_usb-*/$1" 2>/dev/null; }
IF=$(ourif) || { echo "no brcmsmac_usb station interface (module loaded?)"; exit 1; }
PHY=$(basename "$(readlink -f "/sys/class/net/$IF/phy80211")")
mark="monitor-$(date +%s)"; echo "$mark" | sudo tee /dev/kmsg >/dev/null

sudo ip link set "$IF" down
iw dev $MON info >/dev/null 2>&1 || sudo iw phy "$PHY" interface add $MON type monitor ${MONFLAGS:+flags $MONFLAGS}
sudo ip link set $MON up
sudo iw dev $MON set channel "$CH"
iw dev $MON info | grep -E 'type|channel'
s0=$(DBG stats | awk '/^sync_calls/{print $2}'); r0=$(DBG stats | awk '/^async_id_81 /{print $2}')
sudo timeout "$SECS" tcpdump -i $MON -w "$CAP" -q 2>&1 | grep -v '^tcpdump: listening'
s1=$(DBG stats | awk '/^sync_calls/{print $2}'); r1=$(DBG stats | awk '/^async_id_81 /{print $2}')
echo "WLC_RECV messages: $((r1 - r0)); sync RPCs during the capture: $((s1 - s0))"
ARGS=(--chan "$CH")
if [ -n "$REF" ]; then iw dev "$REF" scan dump > /tmp/bmu-ref-scan.txt; ARGS+=(--iw /tmp/bmu-ref-scan.txt); fi
python3 tools/pcapsum.py "$CAP" "${ARGS[@]}"
sudo ip link set $MON down
sudo iw dev $MON del
echo "kernel log problems: $(sudo dmesg | sed -n "/$mark/,\$p" | grep -cE 'Oops|BUG|WARNING|hung_task|Call Trace')"
sudo dmesg | sed -n "/$mark/,\$p" | grep -v nouveau | grep brcmsmac | sort | uniq -c | sort -rn | head -5
