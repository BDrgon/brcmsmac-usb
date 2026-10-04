#!/usr/bin/env python3
"""Per-rpc_id statistics from a USBPcap capture: counts, argument/return sizes, examples, names.

usage: idtable.py capture.pcap DEV [--markers markers.txt]
  --markers  optional file of '[ISO-timestamp] text' lines (as written by the Windows capture
             scripts); each marker starts a phase named by its first two words, used to group counts.
"""
import os, sys, re, datetime
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rpcdecode, rpcids
from collections import defaultdict, Counter


def load_phases(path):
    ph = [('start', 0.0)]
    for line in open(path, encoding='utf-8', errors='replace'):
        m = re.match(r'\[([^\]]+)\]\s+(.*)', line.strip())
        if m:
            t = datetime.datetime.fromisoformat(m.group(1)[:26] + m.group(1)[27:] if len(m.group(1)) > 32 else m.group(1))
            ph.append((' '.join(m.group(2).split()[:2]), t.timestamp()))
    return ph


def main():
    path, dev = sys.argv[1], int(sys.argv[2])
    phases = load_phases(sys.argv[sys.argv.index('--markers') + 1]) if '--markers' in sys.argv else [('all', 0.0)]

    def phase(t):
        p = phases[0][0]
        for n, st in phases:
            if t >= st:
                p = n
        return p

    stats = defaultdict(lambda: dict(n=0, ph=Counter(), alen=Counter(), rlen=Counter(), ex=None, rex=None, ty=Counter()))
    pending = []          # SYNC calls awaiting a ctl09 reply, FIFO (reply xid is the dongle's own counter)
    for x in rpcdecode.rpcs(path, dev):
        if not x['ok'] or x['type'] == 'MGMT' or x['id'] is None:
            continue
        if x['ch'] == 'ctl09':
            if pending:
                s = stats[('H>D', pending.pop(0))]
                s['rlen'][len(x['body']) - 4] += 1
                s['rex'] = s['rex'] or x['body'][4:40].hex()
            continue
        s = stats[(x['dir'], x['id'])]
        s['n'] += 1; s['ph'][phase(x['t'])] += 1; s['alen'][len(x['body']) - 4] += 1; s['ty'][x['type']] += 1
        s['ex'] = s['ex'] or x['body'][4:40].hex()
        if x['type'] == 'SYNC' and x['dir'] == 'H>D':
            pending.append(x['id'])
    for (d, i), s in sorted(stats.items()):
        print(f"{d} id={i:3d} {rpcids.name(i) or '?':40} n={s['n']:5d} {'/'.join(s['ty'])} "
              f"args={dict(s['alen'].most_common(3))} ret={dict(s['rlen'].most_common(2))} "
              f"ph={dict(s['ph'])} ex={s['ex']} rex={s['rex']}")


if __name__ == '__main__':
    main()
