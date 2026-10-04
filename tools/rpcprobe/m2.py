#!/usr/bin/env python3
"""Milestone 2: first RPC call/return round-trips with the BCM4322 BMAC dongle (WNA3100 v1).

Read-only. Mirrors the first calls Windows makes in run2 (PROTOCOL.md §4, §6, §9):
  [download 5.100 TRX + DL_GO if still in ROM] -> DL_RESETCFG (req 8)
  -> HELLO -> CONNECT(chip) -> sync calls 75 REVINFO_GET, 76 STATE_GET, 77 XMTFIFO_SZ_GET x6,
     91 COPYFROM_VARS, 56 HW_ETHERADDR, [--reg] 1 REG_READ(0x688, 2)
Each sync call goes out on bulk EP 0x03; its return is read with control IN bRequest 9 (wLength 1928).
Recovery from any hang: unplug/replug the dongle (firmware lives in RAM only).

usage: sudo python3 m2.py [--fw PATH] [--expect-mac aa:bb:cc:dd:ee:ff] [--reg]
  --fw          firmware TRX (default: /lib/firmware/brcm/bcm4322-bmac-5.100.148.5.trx, then
                ./bcm4322-bmac-5.100.148.5.trx); produce it with tools/fwextract.
  --expect-mac  also check HW_ETHERADDR against this MAC (printed either way).
  --reg         add REG_READ(0x688, 2) -- HANGS the dongle before BMAC_INIT (replug to recover).
Needs: python3-usb (pyusb), root, and tools/rdl.py next to this directory or alongside it.
"""
import os, sys, struct, time
import usb.core, usb.util

_here = os.path.dirname(os.path.abspath(__file__))
sys.path[:0] = [_here, os.path.dirname(_here)]   # rdl.py alongside, or in tools/
import rdl

VID, PID = 0x0846, 0x9020
EP_OUT, EP_IN = 0x03, 0x82
K_CALL, K_SYNC, K_MGMT = 1, 2, 3      # message kinds (docs/protocol.md §4.1)
SESSION = 0x69                      # Windows' choice in run2
VERSION = 0x05649405                # 5.100.148.5 — must match the firmware exactly
CHIP_ROM, CHIP_RUNNING = 0x4322, 0xA123
# session-management actions/status as observed (our labels, docs/protocol.md §4.2)
MGMT_ACTION = {1: 'HELLO', 2: 'CONNECT', 3: 'ACCEPT', 5: 'REFUSE'}
MGMT_STATUS = {0: 'ok', 1: 'hello', 2: 'reconnect', 3: 'version-mismatch'}

# Expected values, as returned to the Windows vendor driver for the same request (run2 capture)
EXPECT = {
    75: lambda p: struct.unpack_from('<I', p, 24)[0] == 0x4322,      # revinfo word 6: chip id (word 0 = 0x14e4 vendor)
    76: lambda p: len(p) == 8,
    91: lambda p: p[4:].startswith(b'manfid=0x846'),
    56: lambda p: len(p) >= 6,
    1:  lambda p: len(p) == 4,
}
NAMES = {75: 'REVINFO_GET', 76: 'STATE_GET', 77: 'XMTFIFO_SZ_GET', 91: 'COPYFROM_VARS',
         56: 'HW_ETHERADDR', 1: 'REG_READ'}


def frame(typ, xid, words):
    rpc = struct.pack('<I', (typ << 24) | (SESSION << 16) | (xid & 0xffff))
    rpc += b''.join(struct.pack('<I', w) for w in words)
    return struct.pack('<I', len(rpc)) + rpc


def parse(buf):
    """One [tp_len][hdr][payload] frame -> (type, session, xid, payload)."""
    n = struct.unpack_from('<I', buf)[0]
    hdr = struct.unpack_from('<I', buf, 4)[0]
    return hdr >> 24, (hdr >> 16) & 0xff, hdr & 0xffff, bytes(buf[8:4 + n])


def chipver(d):
    return struct.unpack('<I', bytes(d.ctrl_transfer(0xC1, 5, 0, 0, 8, timeout=1000))[:4])[0]


def mgn(d, action, w3):
    d.write(EP_OUT, frame(K_MGMT, 0, [action, VERSION, w3]), timeout=2000)
    typ, ses, _, p = parse(bytes(d.read(EP_IN, 2048, timeout=2000)))
    act, ver, reason = struct.unpack_from('<III', p)
    chip = struct.unpack_from('<I', p, 12)[0] if len(p) >= 16 else None
    print(f'  MGMT {MGMT_ACTION[action]:8} -> {MGMT_ACTION.get(act, act)}, ver 0x{ver:08x}, '
          f'status {MGMT_STATUS.get(reason, reason)}, chip {hex(chip) if chip else "-"}')
    return act, chip


def call(d, xid, rpc_id, args=()):
    d.write(EP_OUT, frame(K_SYNC, xid, [rpc_id, *args]), timeout=2000)
    ret = bytes(d.ctrl_transfer(0xC1, 9, 0, 0, 1928, timeout=2000))
    typ, ses, rxid, p = parse(ret)
    echo = struct.unpack_from('<I', p)[0]
    return typ, ses, echo, p[4:]


def main():
    fw = sys.argv[sys.argv.index('--fw') + 1] if '--fw' in sys.argv else next(
        (p for p in ('/lib/firmware/brcm/bcm4322-bmac-5.100.148.5.trx',
                     os.path.join(_here, 'bcm4322-bmac-5.100.148.5.trx')) if os.path.exists(p)), None)
    want_mac = sys.argv[sys.argv.index('--expect-mac') + 1].lower() if '--expect-mac' in sys.argv else None
    d = usb.core.find(idVendor=VID, idProduct=PID)
    if d is None:
        sys.exit('dongle not found')

    v = chipver(d)
    print(f'DL_GETVER: 0x{v:x}')
    if v == CHIP_ROM:
        if not fw:
            sys.exit('no firmware found; pass --fw (see tools/fwextract)')
        print(f'boot ROM — downloading {os.path.basename(fw)} + DL_GO')
        rdl.load(d, fw, True)
        time.sleep(0.5)
        v = chipver(d)
        print(f'DL_GETVER: 0x{v:x}')
    if v != CHIP_RUNNING:
        sys.exit('firmware not running (expected 0xa123) — replug and retry')

    d.ctrl_transfer(0xC1, 8, 0, 0, 8, timeout=1000)   # DL_RESETCFG, as Windows does once after DL_GO

    print('handshake:')
    act, chip = mgn(d, 1, 0)                           # HELLO
    act, _ = mgn(d, 2, chip or 0x4322)                 # CONNECT (w3 = chip id from HELLO reply)
    if act != 3:
        sys.exit('CONNECT not accepted')

    plan = [(75, ()), (76, ())] + [(77, (f,)) for f in range(6)] + [(91, ()), (56, ())]
    if '--reg' in sys.argv:
        # WARNING: before BMAC_INIT the d11 core is held in reset; REG_READ then hangs the
        # whole dongle (even DL_GETVER times out) until replug. Observed 2026-10-04.
        plan.append((1, (0x688, 2)))

    print('calls:')
    ok = True
    fifos = []
    for xid, (rpc_id, args) in enumerate(plan):
        typ, ses, echo, p = call(d, xid, rpc_id, args)
        good = typ == K_SYNC and ses == SESSION and echo == rpc_id
        if rpc_id in EXPECT:
            good = good and EXPECT[rpc_id](p)
        if rpc_id == 56 and want_mac:
            good = good and ':'.join(f'{b:02x}' for b in p[:6]) == want_mac
        ok &= good
        if rpc_id == 77:
            fifos.append(struct.unpack_from('<I', p)[0])
        if rpc_id == 91:
            shown = p[4:].split(b'\0')[:6]
            detail = ' '.join(s.decode(errors='replace') for s in shown) + ' …'
        elif rpc_id == 56:
            detail = ':'.join(f'{b:02x}' for b in p[:6])
        elif rpc_id == 1:
            detail = f'0x688 = 0x{struct.unpack_from("<I", p)[0]:04x}'
        else:
            detail = p[:32].hex()
        argstr = ','.join(hex(a) if a > 9 else str(a) for a in args)
        print(f'  {"PASS" if good else "FAIL"}  {rpc_id:3} {NAMES[rpc_id]}({argstr}) '
              f'-> {len(p)} B  {detail}')
    print(f'  tx fifo sizes: {fifos}  (Windows saw [20, 192, 192, 21, 17, 5])')
    print('\nMILESTONE 2:', 'PASS' if ok else 'FAIL')


if __name__ == '__main__':
    main()
