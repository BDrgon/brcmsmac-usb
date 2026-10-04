#!/usr/bin/env python3
"""Broadcom USB remote-download (RDL) client for the BCM4322 boot ROM (needs pyusb, root).

Behaviour follows the documented brcmfmac USB download protocol. RAM-only: loads a TRX image
into the dongle and optionally starts it; a replug or USB reset returns it to the boot ROM. Usage:
  rdl.py info                 # DL_GETVER + DL_GETSTATE
  rdl.py load <fw.trx> [--go] # DL_START, bulk chunks, poll state, optional DL_GO
"""
import sys, struct, time, usb.core, usb.util
VID, PID = 0x0846, 0x9020
DL_GETSTATE, DL_CHECK_CRC, DL_GO, DL_START, DL_REBOOT, DL_GETVER = 0, 1, 2, 3, 4, 5
STATES = {0:'WAITING',1:'READY',2:'BAD_HDR',3:'BAD_CRC',4:'RUNNABLE',5:'START_FAIL',6:'NVRAM_TOOBIG',7:'IMAGE_TOOBIG'}
CHUNK = 1500  # TRX_RDL_CHUNK
REQTYPE = 0xC1  # USB_DIR_IN | USB_TYPE_VENDOR | USB_RECIP_INTERFACE

def dev():
    d = usb.core.find(idVendor=VID, idProduct=PID)
    if d is None: sys.exit('device 0846:9020 not found')
    try:
        if d.is_kernel_driver_active(0): sys.exit('a kernel driver is bound; unbind first')
    except usb.core.USBError: pass
    return d

def cmd(d, c, n):
    return bytes(d.ctrl_transfer(REQTYPE, c, 0, 0, n, timeout=2000))

def state(d):
    s, b = struct.unpack('<II', cmd(d, DL_GETSTATE, 8)); return s, b

def info(d):
    v = cmd(d, DL_GETVER, 24)
    print(f'DL_GETVER raw ({len(v)} bytes): {v.hex()}')
    w = struct.unpack(f'<{len(v)//4}I', v[:len(v)//4*4])
    names = ['chip','chiprev','ramsize','remapbase','boardtype','boardrev']
    print('bootrom:', ' '.join(f'{k}=0x{x:x}' for k, x in zip(names, w)))
    s, b = state(d); print(f'state={STATES.get(s,s)} bytes={b}')

def load(d, path, go):
    fw = open(path, 'rb').read(); n = len(fw)
    s, b = struct.unpack('<II', cmd(d, DL_START, 8))
    print(f'DL_START -> state={STATES.get(s,s)} bytes={b}')
    if s != 0: sys.exit('not in WAITING state; replug dongle to reset')
    sent = 0; t0 = time.time(); stall = 0
    while b != n:
        if b == sent and sent != n:
            chunk = fw[sent:sent+CHUNK]
            d.write(0x03, chunk, timeout=5000); sent += len(chunk); stall = 0
        s, b = state(d)
        if s in (2, 3, 5, 6, 7): print(f'error state {STATES[s]} at bytes={b} sent={sent}'); return
        stall += 1
        if stall > 2000: print(f'stalled: state={STATES.get(s,s)} bytes={b} sent={sent}'); return
    print(f'downloaded {n} bytes in {time.time()-t0:.1f}s')
    for _ in range(50):
        s, b = state(d)
        if s == 4: break
        time.sleep(0.1)
    print(f'post-download state={STATES.get(s,s)} bytes={b}')
    if go and s == 4:
        try: r = cmd(d, DL_GO, 8); print('DL_GO ->', r.hex())
        except usb.core.USBError as e: print('DL_GO (device likely re-enumerating):', e)

if __name__ == '__main__':
    d = dev()
    if sys.argv[1] == 'info': info(d)
    elif sys.argv[1] == 'load': load(d, sys.argv[2], '--go' in sys.argv)
