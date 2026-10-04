#!/usr/bin/env bash
# Association test inside a network namespace, so the host's own routes, ARP and resolver are never
# touched: the brcmsmac_usb phy is moved into netns "bcmtest", wpa_supplicant (own control dir, no
# D-Bus) and dhcpcd (no hook scripts: no resolv.conf/hostname/NTP changes) run there, then the
# DHCP gateway is pinged. Transmission (allow_tx) is enabled only for the duration of the test.
# Always cleans up: phy back to the initial namespace, netns deleted, allow_tx=0.
#
# WPA_CONF: a wpa_supplicant network block file (mode 600, outside the repo; holds credentials).
# usage: WPA_CONF=/tmp/bmu/wpa.conf tests/hw/netns-assoc.sh [PING_SECONDS] [ATTEMPTS]
set -uo pipefail
PINGS=${1:-60}; ATTEMPTS=${2:-3}
NS=bcmtest; CTRL=/run/bmu-wpa; RUN=/tmp/bmu
: "${WPA_CONF:?set WPA_CONF}"
ourif() { local n; for n in /sys/class/net/*; do
	[ "$(basename "$(readlink -f "$n/device/driver" 2>/dev/null)")" = brcmsmac_usb ] &&
		[ "$(cat "$n/type")" = 1 ] && basename "$n" && return 0; done; return 1; }
nsx() { sudo ip netns exec $NS "$@"; }
DBG() { sudo sh -c "cat /sys/kernel/debug/brcmsmac_usb-*/$1" 2>/dev/null; }
# hide MACs and IPs in what we print
san() { sed -E 's/([0-9a-f]{2}:){5}[0-9a-f]{2}/<mac>/g; s/\b([0-9]{1,3}\.){3}[0-9]{1,3}\b/<ip>/g; s/^ssid=.*/ssid=<ssid>/; s/ssid="[^"]*"/ssid=<ssid>/g; s/SSID=\x27[^\x27]*\x27/SSID=<ssid>/g; s/(id_str|bssid|p2p_device_address|uuid|address)=.*/\1=<redacted>/'; }

IF=$(ourif) || { echo "no brcmsmac_usb interface"; exit 1; }
PHY=$(basename "$(readlink -f "/sys/class/net/$IF/phy80211")")
mkdir -p -m 700 $RUN
mark="netns-assoc-$(date +%s)"; echo "$mark" | sudo tee /dev/kmsg >/dev/null

cleanup() {
	sudo sh -c 'cat /sys/kernel/debug/brcmsmac_usb_wl/*/txlog' > $RUN/txlog.txt 2>/dev/null
	sudo sh -c 'cat /sys/kernel/debug/brcmsmac_usb_wl/*/last_tx' > $RUN/last_tx.txt 2>/dev/null
	sudo sh -c 'cat /sys/kernel/debug/brcmsmac_usb_wl/*/rxtx' 2>/dev/null
	sudo sh -c "for k in /sys/kernel/debug/ieee80211/$PHY/keys/*; do [ -d \$k ] || continue;
		echo \"mac80211 key \$(basename \$k): \$(cat \$k/algorithm) keyidx \$(cat \$k/keyidx) icverrors \$(cat \$k/icverrors 2>/dev/null) replays \$(cat \$k/replays 2>/dev/null) mic_failures \$(cat \$k/mic_failures 2>/dev/null)\"; done" 2>/dev/null
	echo 0 | sudo tee /sys/module/brcmsmac_usb/parameters/allow_tx >/dev/null
	[ -f $RUN/dhcpcd.pid ] && sudo kill "$(cat $RUN/dhcpcd.pid)" 2>/dev/null
	sudo pkill -f "dhcpcd.*$IF" 2>/dev/null
	[ -f $RUN/wpa.pid ] && sudo kill "$(cat $RUN/wpa.pid)" 2>/dev/null
	sleep 1
	if sudo ip netns list | grep -q "^$NS"; then
		nsx iw phy "$PHY" set netns 1 2>/dev/null
		sudo ip netns del $NS
	fi
	sudo rm -f $RUN/wpa.pid $RUN/dhcpcd.pid
	echo "cleanup: allow_tx=$(cat /sys/module/brcmsmac_usb/parameters/allow_tx), netns: $(sudo ip netns list | grep -c "^$NS"), phy back: $(iw phy "$PHY" info >/dev/null 2>&1 && echo yes || echo no)"
}
trap cleanup EXIT

sudo ip link set "$IF" down
sudo ip netns add $NS
sudo iw phy "$PHY" set netns name $NS || exit 1
nsx ip link set lo up
nsx ip link set "$IF" up
echo 1 | sudo tee /sys/module/brcmsmac_usb/parameters/allow_tx >/dev/null

{ echo "ctrl_interface=$CTRL"; echo "ap_scan=1"; sudo cat "$WPA_CONF"; } | sudo tee $RUN/wpa-run.conf >/dev/null
sudo chmod 600 $RUN/wpa-run.conf
t0=$(date +%s)
nsx wpa_supplicant -B -D nl80211 -i "$IF" -c $RUN/wpa-run.conf -P $RUN/wpa.pid -t -f $RUN/wpa.log 2>&1 | san
ok=0
for a in $(seq 1 "$ATTEMPTS"); do
	for _ in $(seq 1 60); do
		st=$(nsx wpa_cli -p $CTRL -i "$IF" status 2>/dev/null | awk -F= '/^wpa_state/{print $2}')
		[ "$st" = COMPLETED ] && { ok=1; break; }
		sleep 1
	done
	[ $ok = 1 ] && break
	echo "attempt $a: wpa_state=$st after 60 s; rescanning (the AP may be asleep/hidden)"
	nsx wpa_cli -p $CTRL -i "$IF" reassociate >/dev/null 2>&1
done
echo "wpa_state=$st after $(( $(date +%s) - t0 )) s"
nsx wpa_cli -p $CTRL -i "$IF" status 2>/dev/null | grep -E '^(wpa_state|freq|key_mgmt|pairwise_cipher|group_cipher|wifi_generation)=' | san
sudo grep -E 'CTRL-EVENT|Trying to (authenticate|associate)|Associated with|WPA: Key negotiation|auth|timed out|reason' $RUN/wpa.log | san | tail -15
[ $ok = 1 ] || { echo "RESULT: no association"; exit 2; }

nsx dhcpcd -4 -1 -t 30 -c /bin/true -p "$IF" >$RUN/dhcpcd.out 2>&1; rc=$?
san < $RUN/dhcpcd.out | tail -5
GW=$(nsx ip -4 route show default | awk '/default/{print $3; exit}')
nsx ip -4 -br addr show "$IF" | san
[ $rc = 0 ] && [ -n "$GW" ] || { echo "RESULT: associated, DHCP failed (rc $rc)"; exit 3; }
echo "DHCP ok; pinging the gateway for $PINGS s"
s0=$(DBG stats | awk '/^async_id_82 /{print $2}')
nsx ping -c "$PINGS" -i 1 -W 2 "$GW" > $RUN/ping.out 2>&1
tail -3 $RUN/ping.out | san
s1=$(DBG stats | awk '/^async_id_82 /{print $2}')
echo "tx statuses during ping: $(( ${s1:-0} - ${s0:-0} ))"
nsx iw dev "$IF" link | grep -E 'freq|signal|bitrate' | san
nsx iw dev "$IF" station dump | grep -E 'tx (packets|retries|failed)|rx packets|signal:|beacon loss|tx bitrate|rx bitrate' | san
echo "kernel log problems: $(sudo dmesg | sed -n "/$mark/,\$p" | grep -cE 'Oops|BUG|WARNING|hung_task|Call Trace')"
sudo dmesg | sed -n "/$mark/,\$p" | grep -v nouveau | grep -E 'brcms|wlp|wlan' | san | sort | uniq -c | sort -rn | head -12
echo "RESULT: ok"
