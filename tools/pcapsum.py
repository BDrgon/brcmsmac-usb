#!/usr/bin/env python3
# SPDX-License-Identifier: ISC
"""Summarise a monitor-mode capture (pcap, radiotap link type 127) for gate checks.

    pcapsum.py CAP.pcap [--chan N] [--iw SCAN.txt] [--rpcprobe RX.json] [--show-bssid]

Reports frame types, radiotap channel vs the channel the capture was tuned to and vs each
beacon's DS Parameter Set IE, signal statistics, TSFT ordering and, with --iw / --rpcprobe,
the per-BSS mean signal against another receiver (`iw dev X scan dump` text, or the JSON written
by `python3 -m rpcprobe rx --out`). BSSIDs and SSIDs are not printed unless --show-bssid is given,
so the output can be pasted into notes.
"""
import argparse
import collections
import json
import re
import statistics
import struct
import sys

TYPES = {0: 'mgmt', 1: 'ctrl', 2: 'data'}
MGMT = {0: 'assoc-req', 1: 'assoc-resp', 4: 'probe-req', 5: 'probe-resp', 8: 'beacon',
        10: 'disassoc', 11: 'auth', 12: 'deauth', 13: 'action'}
CTRL = {8: 'block-ack-req', 9: 'block-ack', 10: 'ps-poll', 11: 'rts', 12: 'cts', 13: 'ack'}

# radiotap fields 0..5: (alignment, size)
RT_FIELDS = [(8, 8), (1, 1), (1, 1), (2, 4), (1, 2), (1, 1)]


def freq2chan(f):
    if f == 2484:
        return 14
    if 2412 <= f <= 2472:
        return (f - 2407) // 5
    if f >= 5000:
        return (f - 5000) // 5
    return None


def radiotap(pkt):
    """Return (header_len, fields dict) for the first radiotap namespace."""
    _, _, hlen, present = struct.unpack_from('<BBHI', pkt, 0)
    off = 8
    p = present
    while p & 0x80000000:			# extended presence bitmaps
        p = struct.unpack_from('<I', pkt, off)[0]
        off += 4
    out = {}
    for bit, (align, size) in enumerate(RT_FIELDS):
        if not present & (1 << bit):
            continue
        off = (off + align - 1) & ~(align - 1)
        if bit == 0:
            out['tsft'] = struct.unpack_from('<Q', pkt, off)[0]
        elif bit == 1:
            out['flags'] = pkt[off]
        elif bit == 2:
            out['rate'] = pkt[off] / 2
        elif bit == 3:
            out['freq'], out['chflags'] = struct.unpack_from('<HH', pkt, off)
        elif bit == 5:
            out['signal'] = struct.unpack_from('<b', pkt, off)[0]
        off += size
    return hlen, out


def ds_channel(body):
    off = 12				# timestamp, interval, capability
    while off + 2 <= len(body):
        eid, n = body[off], body[off + 1]
        if eid == 3 and n >= 1 and off + 2 < len(body):
            return body[off + 2]
        off += 2 + n
    return None


def ssid(body):
    if len(body) >= 14 and body[12] == 0:
        return body[14:14 + body[13]].decode('utf-8', 'replace')
    return None


def read_pcap(path):
    with open(path, 'rb') as f:
        data = f.read()
    magic = struct.unpack_from('<I', data, 0)[0]
    if magic not in (0xa1b2c3d4, 0xa1b23c4d):
        sys.exit('not a little-endian pcap file')
    linktype = struct.unpack_from('<I', data, 20)[0]
    if linktype != 127:
        sys.exit(f'link type {linktype}, expected 127 (radiotap)')
    off = 24
    while off + 16 <= len(data):
        _, _, incl, _ = struct.unpack_from('<IIII', data, off)
        yield data[off + 16:off + 16 + incl]
        off += 16 + incl


def parse_iw(path):
    """BSSID -> signal (dBm) from `iw dev X scan dump` output."""
    out, cur = {}, None
    for line in open(path):
        m = re.match(r'BSS ([0-9a-f:]{17})', line)
        if m:
            cur = m.group(1)
            continue
        m = re.match(r'\s+signal: (-?[0-9.]+) dBm', line)
        if m and cur:
            out[cur] = float(m.group(1))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('pcap')
    ap.add_argument('--chan', type=int, help='channel the monitor interface was tuned to')
    ap.add_argument('--iw', help='`iw dev X scan dump` output from another receiver')
    ap.add_argument('--rpcprobe', help='JSON from `rpcprobe rx --out`')
    ap.add_argument('--show-bssid', action='store_true')
    a = ap.parse_args()

    kinds = collections.Counter()
    freqs = collections.Counter()
    rates = collections.Counter()
    signals = []
    ds_match = ds_mismatch = 0
    ds_pairs = collections.Counter()
    zero_sig = collections.Counter()
    bad_fcs = 0
    tsft_back = 0
    last_tsft = None
    bss = {}
    n = 0
    for pkt in read_pcap(a.pcap):
        n += 1
        hlen, rt = radiotap(pkt)
        fr = pkt[hlen:]
        if len(fr) < 2:
            kinds['runt'] += 1
            continue
        if rt.get('flags', 0) & 0x40:	# bad FCS (monitor flag fcsfail): content unreliable
            bad_fcs += 1
            continue
        fc = struct.unpack_from('<H', fr, 0)[0]
        ftype, sub = (fc >> 2) & 3, (fc >> 4) & 15
        name = TYPES.get(ftype, 'ext')
        if ftype == 0:
            name += '/' + MGMT.get(sub, str(sub))
        elif ftype == 1:
            name += '/' + CTRL.get(sub, str(sub))
        elif ftype == 2:
            name += '/qos' if sub & 8 else '/plain'
            if sub & 4:
                name += '-null'
        kinds[name] += 1
        if 'freq' in rt:
            freqs[rt['freq']] += 1
        if 'rate' in rt:
            rates[rt['rate']] += 1
        if 'signal' in rt:
            signals.append(rt['signal'])
            if rt['signal'] >= 0:
                zero_sig[name] += 1
        if 'tsft' in rt:
            if last_tsft is not None and rt['tsft'] < last_tsft:
                tsft_back += 1
            last_tsft = rt['tsft']
        if ftype == 0 and sub == 8 and len(fr) >= 24:
            body = fr[24:]
            ds = ds_channel(body)
            ch = freq2chan(rt.get('freq', 0))
            if ds is not None:
                if ds == ch:
                    ds_match += 1
                else:
                    ds_mismatch += 1
                    ds_pairs[(ch, ds)] += 1
            bssid = ':'.join(f'{b:02x}' for b in fr[16:22])
            e = bss.setdefault(bssid, {'n': 0, 'sig': [], 'ds': ds, 'ssid': ssid(body)})
            e['n'] += 1
            if 'signal' in rt:
                e['sig'].append(rt['signal'])

    print(f'frames: {n}')
    for k, v in sorted(kinds.items(), key=lambda kv: -kv[1]):
        print(f'  {k:22s} {v}')
    print('radiotap frequency: ' + ', '.join(f'{f} MHz (ch {freq2chan(f)}): {c}' for f, c in freqs.items()))
    if a.chan is not None:
        off = sum(c for f, c in freqs.items() if freq2chan(f) != a.chan)
        print(f'frames with a radiotap channel other than the tuned channel {a.chan}: {off}')
    print(f'beacons whose DS IE matches the radiotap channel: {ds_match}, mismatching: {ds_mismatch}'
          + (' (' + ', '.join(f'rx ch {c} / DS {d}: {k}' for (c, d), k in ds_pairs.items()) + ')' if ds_pairs else ''))
    if zero_sig:
        print('frames with signal >= 0 dBm (no valid RSSI): ' + ', '.join(f'{k} {v}' for k, v in zero_sig.items()))
    if signals:
        print(f'signal dBm: min {min(signals)} median {statistics.median(signals)} max {max(signals)} '
              f'(frames with a signal field: {len(signals)}/{n})')
    print('rates (Mbit/s): ' + ', '.join(f'{r:g}: {c}' for r, c in sorted(rates.items())))
    print(f'bad-FCS frames (excluded above): {bad_fcs}; TSFT going backwards: {tsft_back}')
    print(f'BSSs (beacons): {len(bss)}')

    others = []
    if a.iw:
        others.append(('iw', {k: [v] for k, v in parse_iw(a.iw).items()}))
    if a.rpcprobe:
        others.append(('rpcprobe', {e['bssid']: e['rssi'] for e in json.load(open(a.rpcprobe))}))
    rows = sorted(bss.items(), key=lambda kv: -statistics.mean(kv[1]['sig'] or [-100]))
    for i, (b, e) in enumerate(rows, 1):
        mean = statistics.mean(e['sig']) if e['sig'] else float('nan')
        label = f'{b} {e["ssid"]!r:28.28}' if a.show_bssid else f'BSS{i:02d}'
        cols = [f'{label}  ds {e["ds"]!s:>3}  n {e["n"]:4d}  ours {mean:6.1f}']
        for name, o in others:
            if b in o:
                om = statistics.mean(o[b])
                cols.append(f'{name} {om:6.1f} (Δ {mean - om:+5.1f})')
            else:
                cols.append(f'{name}    -')
        print('  ' + '  '.join(cols))
    for name, o in others:
        d = [statistics.mean(e['sig']) - statistics.mean(o[b]) for b, e in bss.items() if b in o and e['sig']]
        if d:
            print(f'vs {name}: {len(d)} common BSSs, mean Δ {statistics.mean(d):+.1f} dB, '
                  f'max |Δ| {max(abs(x) for x in d):.1f} dB, within ±10 dB: {sum(abs(x) <= 10 for x in d)}/{len(d)}')


if __name__ == '__main__':
    main()
