#!/usr/bin/env python3
# SPDX-License-Identifier: ISC
"""Compare the driver's last TX buffer (debugfs brcmsmac_usb_wl/<dev>/last_tx) with what
tools/rpcprobe/tx.py builds for the same 802.11 frame: rpcprobe reproduces the vendor driver's
d11txh byte for byte for CCK management frames (docs/protocol.md §7.1).

    sudo cat /sys/kernel/debug/brcmsmac_usb_wl/*/last_tx | python3 tools/dev/txcmp.py

Prints every 16-bit d11txh word that differs, by brcmsmac field name. Exit status 1 if any do.
"""
import os
import re
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
from rpcprobe import tx  # noqa: E402

# brcmsmac d11.h struct d11txh, by 16-bit word index
FIELDS = {0x00: 'MacTxControlLow', 0x01: 'MacTxControlHigh', 0x02: 'MacFrameControl',
          0x03: 'TxFesTimeNormal', 0x04: 'PhyTxControlWord', 0x05: 'PhyTxControlWord_1',
          0x06: 'PhyTxControlWord_1_Fbr', 0x07: 'PhyTxControlWord_1_Rts',
          0x08: 'PhyTxControlWord_1_FbrRts', 0x09: 'MainRates', 0x0a: 'XtraFrameTypes',
          0x0b: 'IV[0]', 0x13: 'TxFrameRA[0]', 0x14: 'TxFrameRA[1]', 0x15: 'TxFrameRA[2]',
          0x16: 'TxFesTimeFallback', 0x17: 'RTSPLCPFallback[0]', 0x1a: 'RTSDurFallback',
          0x1b: 'FragPLCPFallback[0]', 0x1c: 'FragPLCPFallback[1]', 0x1d: 'FragPLCPFallback[2]',
          0x1e: 'FragDurFallback', 0x1f: 'MModeLen', 0x20: 'MModeFbrLen', 0x21: 'TstampLow',
          0x22: 'TstampHigh', 0x23: 'ABI_MimoAntSel', 0x24: 'PreloadSize', 0x25: 'AmpduSeqCtl',
          0x26: 'TxFrameID', 0x27: 'TxStatus', 0x28: 'MaxNMpdus', 0x29: 'MaxABytes_MRT',
          0x2a: 'MaxABytes_FBR', 0x2b: 'MinMBytes', 0x2c: 'RTSPhyHeader[0]', 0x2f: 'rts_frame[0]'}


def main():
    text = sys.stdin.read()
    head = text.splitlines()[0]
    m = re.match(r'fifo (\d+) len (\d+)', head)
    if not m:
        sys.exit('unexpected input: ' + head)
    fifo, total = int(m.group(1)), int(m.group(2))
    buf = bytearray()
    for line in text.splitlines()[1:]:
        hexpart = line.split(':', 1)[1].strip() if ':' in line else ''
        buf += bytes.fromhex(''.join(hexpart.split()[:16]))
    if len(buf) < total:
        sys.exit(f'buffer truncated ({len(buf)}/{total} B); frame too long to compare')
    txh, plcp, frame = buf[:112], bytes(buf[112:118]), bytes(buf[118:total])
    w = lambda b, i: struct.unpack_from('<H', b, i * 2)[0]
    fc = struct.unpack_from('<H', frame)[0]
    unicast = frame[4] & 1 == 0
    chan = w(txh, 0x0a) >> 8
    print(f'fifo {fifo}, {len(frame)} B frame, FC 0x{fc:04x}, {"unicast" if unicast else "group"}, '
          f'channel {chan}, PLCP {plcp.hex()}, TxFrameID 0x{w(txh, 0x26):04x}')
    ref = tx.d11txh(frame, plcp, chan, w(txh, 0x26), unicast, duration=w(txh, 0x1e))
    diffs = 0
    for i in range(56):
        a, b = w(txh, i), w(ref, i)
        if a != b:
            diffs += 1
            name = FIELDS.get(i, f'word 0x{i:02x}')
            print(f'  {name:26s} driver 0x{a:04x}  rpcprobe/vendor 0x{b:04x}')
    print('d11txh identical' if not diffs else f'{diffs} word(s) differ')
    sys.exit(1 if diffs else 0)


if __name__ == '__main__':
    main()
