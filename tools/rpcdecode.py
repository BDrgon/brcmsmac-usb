#!/usr/bin/env python3
"""Decode BCM4322 BMAC RPC traffic from a USBPcap capture (Windows vendor driver).

usage: rpcdecode.py capture.pcap DEV [--from HH:MM:SS] [--to HH:MM:SS] [--names]
  DEV  USB device address of the dongle in the capture (see `usbpcap.py capture.pcap`).
Emits one line per RPC frame: local time, dir, channel (bulk03 = host->dongle, bulk82 = dongle->host
async, ctl09 = sync-call reply via control IN bRequest 9), type, session, xid, rpc_id, payload length,
first 48 payload bytes after the id. Handles transport aggregation; skips the RDL download phase.
Timestamps are shown in the local timezone of the machine running the decoder."""
import os, sys, struct, datetime
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import usbpcap, rpcids
TYPES = {1: 'CALL', 2: 'SYNC', 3: 'MGMT'}   # message kinds, see docs/protocol.md §4.1
def frames(buf):
    """split a TP buffer into RPC frames (handles aggregation)."""
    off = 0
    while off + 4 <= len(buf):
        n = struct.unpack_from('<I', buf, off)[0]
        if n < 4 or off + 4 + n > len(buf):
            yield ('BAD', buf[off:]); return
        yield ('OK', buf[off+4:off+4+n]); off += 4 + n
def records(path, dev):
    pend = {}
    for r in usbpcap.read(path):
        if r['dev'] != dev: continue
        if r['xfer'] == 2 and r['setup'] is not None: pend[r['irp']] = r['setup']; continue
        if r['xfer'] == 2 and r['from_dev']:
            s = pend.pop(r['irp'], None)
            if s and s[0] == 0xC1 and s[1] == 9 and r['data']: yield r, 'ctl09', 'D>H', r['data']
            continue
        if r['xfer'] == 3:
            if r['ep'] == 0x03 and not r['from_dev'] and r['data']: yield r, 'bulk03', 'H>D', r['data']
            if r['ep'] == 0x82 and r['from_dev'] and r['data']: yield r, 'bulk82', 'D>H', r['data']
def rpcs(path, dev, start=None):
    """yield dicts; skips the RDL download (bulk03 before the first session-management message)."""
    started = False
    for r, ch, d, buf in records(path, dev):
        if not started:
            if ch == 'bulk03' and len(buf) == 20 and buf[:4] == b'\x10\0\0\0' and buf[7] == 3: started = True
            else: continue
        for st, f in frames(buf):
            rec = dict(t=r['t'], ch=ch, dir=d, raw=f, ok=st == 'OK', xferlen=len(buf), rec=r['i'])
            if st == 'OK' and len(f) >= 4:
                h = struct.unpack_from('<I', f)[0]
                rec.update(type=TYPES.get(h >> 24, h >> 24), session=(h >> 16) & 0xff, xid=h & 0xffff, body=f[4:])
                rec['id'] = struct.unpack_from('<I', f, 4)[0] if len(f) >= 8 else None
            yield rec
def ts(t): return datetime.datetime.fromtimestamp(t).strftime('%H:%M:%S.%f')[:-3]
if __name__ == '__main__':
    path, dev = sys.argv[1], int(sys.argv[2])
    fr = sys.argv[sys.argv.index('--from')+1] if '--from' in sys.argv else '00'
    to = sys.argv[sys.argv.index('--to')+1] if '--to' in sys.argv else '99'
    for x in rpcs(path, dev):
        t = ts(x['t'])
        if not (fr <= t <= to): continue
        if not x['ok']: print(t, x['ch'], 'BADFRAME', x['raw'][:32].hex()); continue
        print(t, x['dir'], x['ch'], x['type'], 's%02x' % x['session'], 'x%04x' % x['xid'],
              ('id=%3d' % x['id']) if x['id'] is not None else '', len(x['body']), x['body'][4:4+48].hex(),
              (rpcids.name(x['id']) or '') if '--names' in sys.argv and x['type'] != 'MGMT' and x['id'] is not None else '')
