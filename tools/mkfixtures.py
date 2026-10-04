#!/usr/bin/env python3
"""Build sanitised unit-test fixtures from a private USBPcap capture of the vendor driver.

usage: mkfixtures.py CAPTURE.pcap DEV -o tests/fixtures/run2_sanitised.json

What is kept, and how it is sanitised (SPEC §6):
  * session handshake frames (firmware version + chip id only)
  * sync replies for REVINFO_GET (75), STATE_GET (76), XMTFIFO_SZ_GET (77)
  * HW_ETHERADDR (56) reply with the MAC replaced by 02:00:00:00:00:01
  * a few WLC_RECV beacons, rebuilt: placeholder MACs, SSID "TESTNET…", only the SSID /
    supported-rates / DS-parameter IEs, RX header lengths fixed up and the FCS recomputed
  * one aggregated dongle->host transfer rebuilt from those sanitised beacons
No NVRAM, no data frames, no other IEs.
"""
import argparse, json, os, struct, sys, zlib
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rpcdecode
from rpcprobe import rx

DONGLE = bytes.fromhex('020000000001')
PLACEHOLDERS = [bytes.fromhex(f'0200000000{i:02x}') for i in range(2, 0x40)]


def sanitise_recv(body, macmap):
    """body = full message body of a WLC_RECV (rpc_id + args). Returns a rebuilt body."""
    args = body[4:]
    ln = struct.unpack_from('<I', args)[0]
    d = bytearray(args[4:4 + ln])
    rs1 = struct.unpack_from('<H', d, 16)[0]
    off = rx.HDR_LEN + (2 if rs1 & rx.RXS_PBPRES else 0)
    pre, f = bytes(d[:off + 6]), bytes(d[off + 6:])
    hdr = bytearray(f[:24])
    for o in (4, 10, 16):
        a = bytes(hdr[o:o + 6])
        if a != b'\xff' * 6:
            if a not in macmap:
                macmap[a] = PLACEHOLDERS[len(macmap)]
            hdr[o:o + 6] = macmap[a]
    fixed = bytearray(f[24:36])
    fixed[0:8] = b'\0' * 8                                   # timestamp
    kept = b''
    for eid, v in rx.ies(f):
        if eid == 0:
            kept += bytes([0, len(v)]) + (b'TESTNET' * 5)[:len(v)]
        elif eid in (1, 3):
            kept += bytes([eid, len(v)]) + v
    nf = bytes(hdr) + bytes(fixed) + kept
    nf += struct.pack('<I', zlib.crc32(nf) & 0xffffffff)
    pre = bytearray(pre)
    struct.pack_into('<H', pre, 0, len(pre) - rx.HDR_LEN + len(nf))   # RxFrameSize
    data = bytes(pre) + nf
    return struct.pack('<II', 81, len(data)) + data


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('capture'); ap.add_argument('dev', type=int)
    ap.add_argument('-o', '--out', required=True)
    a = ap.parse_args()
    fx = dict(source='vendor-driver capture (sanitised; see tools/mkfixtures.py)', mgmt=[],
              replies={}, beacons=[])
    macmap = {}
    pending = []
    for x in rpcdecode.rpcs(a.capture, a.dev):
        if not x['ok']:
            continue
        if x['type'] == 'MGMT' and len(fx['mgmt']) < 4:
            fx['mgmt'].append(dict(dir=x['dir'], raw=x['raw'].hex()))
        elif x['dir'] == 'H>D' and x['type'] == 'SYNC':
            pending.append(x)
        elif x['ch'] == 'ctl09' and pending:
            call = pending.pop(0)
            rid = x['id']
            raw = bytearray(x['raw'])
            if rid == 56:
                raw[8:14] = DONGLE
            if rid in (56, 75, 76, 77) and str(rid) not in fx['replies']:
                fx['replies'][str(rid)] = dict(call_args=call['body'][4:].hex(),
                                                reply=(struct.pack('<I', len(raw)) + bytes(raw)).hex())
        elif x['ch'] == 'bulk82' and x['id'] == 81 and len(fx['beacons']) < 4:
            f = rx.parse_recv(x['body'][4:])
            if rx.beacon_info(f) and f.fcs_ok and f.ftype == 0 and f.subtype == 8:
                hdr = x['raw'][:4]
                fx['beacons'].append((hdr + sanitise_recv(x['body'], macmap)).hex())
    # one aggregated transfer of 3 sanitised beacons
    msgs = [bytes.fromhex(b) for b in fx['beacons'][:3]]
    fx['agg_transfer'] = b''.join(struct.pack('<I', len(m)) + m for m in msgs).hex()
    os.makedirs(os.path.dirname(a.out) or '.', exist_ok=True)
    json.dump(fx, open(a.out, 'w'), indent=1)
    print(f'{a.out}: {len(fx["mgmt"])} mgmt, replies {sorted(fx["replies"])}, '
          f'{len(fx["beacons"])} beacons, {len(macmap)} MACs replaced')


if __name__ == '__main__':
    main()
