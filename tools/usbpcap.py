#!/usr/bin/env python3
"""Minimal USBPcap (linktype 249) pcap reader. No dependencies.

usage: usbpcap.py capture.pcap      # per (device, endpoint, transfer type, direction) packet counts
Library: read(path) yields dicts: t (unix float), irp, status, func, info, bus, dev, ep, xfer, dlen, stage, setup, data, dir_in (bool: completion from device)."""
import struct, sys
XFER = {0:'ISO',1:'INT',2:'CTRL',3:'BULK'}
def read(path):
    d = open(path,'rb').read()
    magic = d[:4]; e = '<' if magic in (b'\xd4\xc3\xb2\xa1', b'\x4d\x3c\xb2\xa1') else '>'
    nsres = magic == b'\x4d\x3c\xb2\xa1'
    off = 24; n = 0
    while off + 16 <= len(d):
        ts, tu, incl, orig = struct.unpack(e+'IIII', d[off:off+16]); off += 16
        p = d[off:off+incl]; off += incl; n += 1
        hl, irp, st, fn, info, bus, dev, ep, xf, dl = struct.unpack('<HQIHBHHBBI', p[:27])
        r = dict(i=n, t=ts + tu/(1e9 if nsres else 1e6), irp=irp, status=st, func=fn, info=info,
                 bus=bus, dev=dev, ep=ep, xfer=xf, dlen=dl, stage=None, setup=None)
        if xf == 2 and hl >= 28:
            r['stage'] = p[27]
        body = p[hl:]
        if xf == 2 and r['stage'] == 0:   # SETUP stage: 8-byte setup packet
            r['setup'] = body[:8]; body = body[8:]
        r['data'] = body
        r['from_dev'] = bool(info & 1)
        yield r
if __name__ == '__main__':
    from collections import Counter
    c = Counter()
    for r in read(sys.argv[1]):
        c[(r['dev'], r['ep'], XFER.get(r['xfer']), 'IN' if r['ep'] & 0x80 else 'OUT', r['from_dev'])] += 1
    for k, v in sorted(c.items()): print(k, v)
