#!/usr/bin/env python3
"""Extract a host call sequence from a USBPcap capture into a replayable JSONL file.

usage: seqextract.py CAPTURE.pcap DEV -o OUT.jsonl [--until-sync ID] [--until-time HH:MM:SS.mmm]
       seqextract.py CAPTURE.pcap DEV --chanblocks OUT.json

--chanblocks writes, per 2.4 GHz channel, the first host call block the vendor driver used to retune:
from SUSPEND_MAC_AND_WAIT (50) through ENABLE_MAC (41) around a BMAC_CHANSPEC_SET (58), with TX
(61) calls removed. Same entry format as below.

Starts after the session handshake (CONNECT accepted) and stops after the first reply to sync
call --until-sync (default 6 = BMAC_UP_FINISH, i.e. "interface up"), or at --until-time.
Lines:
  {"meta": {...}, "mac": "<dongle MAC from HW_ETHERADDR>"}                      first line
  {"op": "call"|"sync", "id": N, "args": "<hex>", "group": G, "expect": "<hex>"}  host calls
  {"op": "async", "id": N, "args": "<hex>", "after": SEQ}                          dongle frames
`args` are verbatim (after the rpc_id); `group` numbers USB transfers (frames sharing a group were
aggregated by the vendor driver); `expect` is the captured reply body after the echoed id.
Output contains device-specific data from your capture: keep it under work/ (gitignored).
"""
import argparse, datetime, json, os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rpcdecode


def host_entries(capture, dev):
    """All host calls after the handshake, with sync replies attached as 'expect'."""
    out, pending, started = [], [], False
    for x in rpcdecode.rpcs(capture, dev):
        if not x['ok']:
            continue
        if not started:
            started = x['type'] == 'MGMT' and x['dir'] == 'D>H' and x['id'] == 3
            continue
        if x['dir'] == 'H>D':
            e = dict(op='sync' if x['type'] == 'SYNC' else 'call', id=x['id'],
                     args=x['body'][4:].hex(), group=x['rec'])
            out.append(e)
            if e['op'] == 'sync':
                pending.append(e)
        elif x['ch'] == 'ctl09' and pending:
            pending.pop(0)['expect'] = x['body'][4:].hex()
    return out


def chanblocks(capture, dev, path):
    ents, blocks = host_entries(capture, dev), {}
    for i, e in enumerate(ents):
        if e['id'] != 58 or e['op'] != 'call':
            continue
        ch = bytes.fromhex(e['args'])[0]
        if ch in blocks or not 1 <= ch <= 14:
            continue
        a = max((j for j in range(i) if ents[j]['id'] == 50), default=None)
        b = min((j for j in range(i, len(ents)) if ents[j]['id'] == 41), default=None)
        if a is None or b is None or any(ents[j]['id'] == 41 for j in range(a, i)):
            continue                     # not a suspend..retune..enable block (e.g. init)
        blocks[ch] = [x for x in ents[a:b + 1] if x['id'] != 61]
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, 'w') as f:
        json.dump({str(k): v for k, v in sorted(blocks.items())}, f)
    print(f'{path}: channels {sorted(blocks)}; '
          f'calls per block {[len(blocks[k]) for k in sorted(blocks)]}')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('capture'); ap.add_argument('dev', type=int)
    ap.add_argument('-o', '--out')
    ap.add_argument('--chanblocks')
    ap.add_argument('--until-sync', type=int, default=6)
    ap.add_argument('--until-time')
    a = ap.parse_args()
    if a.chanblocks:
        return chanblocks(a.capture, a.dev, a.chanblocks)
    if not a.out:
        ap.error('-o is required')

    out, pending, mac = [], [], None
    group, last_xfer, started, last_seq = 0, None, False, -1
    for x in rpcdecode.rpcs(a.capture, a.dev):
        if not x['ok']:
            sys.exit(f'framing error in capture at {rpcdecode.ts(x["t"])}')
        if a.until_time and rpcdecode.ts(x['t']) > a.until_time:
            break
        if not started:
            if x['type'] == 'MGMT' and x['dir'] == 'D>H' and x['id'] == 3:
                started = True
            continue
        args = x['body'][4:].hex()
        if x['dir'] == 'H>D':
            key = x['rec']                        # pcap record = one USB transfer
            if key != last_xfer:
                group += 1
                last_xfer = key
            op = 'sync' if x['type'] == 'SYNC' else 'call'
            e = dict(op=op, id=x['id'], args=args, group=group)
            out.append(e)
            last_seq = len(out) - 1
            if op == 'sync':
                pending.append(e)
        elif x['ch'] == 'ctl09':
            e = pending.pop(0)
            if e['id'] != x['id']:
                sys.exit(f'reply id {x["id"]} does not match pending call {e["id"]}')
            e['expect'] = args
            if x['id'] == 56:
                mac = ':'.join(f'{b:02x}' for b in x['body'][4:10])
            if x['id'] == a.until_sync:
                break
        else:
            out.append(dict(op='async', id=x['id'], args=args, after=last_seq))

    meta = dict(source=os.path.basename(a.capture), until_sync=a.until_sync,
                calls=sum(e['op'] != 'async' for e in out),
                syncs=sum(e['op'] == 'sync' for e in out),
                extracted=datetime.datetime.now().isoformat(timespec='seconds'))
    fd = os.open(a.out, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, 'w') as f:
        f.write(json.dumps(dict(meta=meta, mac=mac)) + '\n')
        for e in out:
            f.write(json.dumps(e) + '\n')
    print(f'{a.out}: {meta["calls"]} host calls ({meta["syncs"]} sync), '
          f'{sum(e["op"] == "async" for e in out)} async frames, mac {"found" if mac else "-"}')


if __name__ == '__main__':
    main()
