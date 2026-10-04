#!/usr/bin/env python3
"""Try to recover a hung BCM4322 dongle without unplugging it (needs root).

usage: usbrecover.py [reset|authorize|status]
  status     print the dongle's DL_GETVER answer (0x4322 boot ROM, 0xa123 firmware running, or hung)
  reset      USB port reset (USBDEVFS_RESET ioctl on /dev/bus/usb/BBB/DDD)
  authorize  sysfs: write 0 then 1 to the device's 'authorized' attribute (re-enumeration)
Both methods keep VBUS on; neither power-cycles the dongle. See docs/recovery.md for results.
"""
import fcntl, glob, os, struct, sys, time

VID, PID = '0846', '9020'
USBDEVFS_RESET = ord('U') << 8 | 20


def sysdev():
    for d in glob.glob('/sys/bus/usb/devices/*'):
        try:
            if open(d + '/idVendor').read().strip() == VID and open(d + '/idProduct').read().strip() == PID:
                return d
        except OSError:
            pass
    return None


def status():
    import usb.core
    d = usb.core.find(idVendor=int(VID, 16), idProduct=int(PID, 16))
    if d is None:
        return 'absent'
    try:
        v = struct.unpack('<I', bytes(d.ctrl_transfer(0xC1, 5, 0, 0, 8, timeout=1000))[:4])[0]
        return {0x4322: 'boot ROM (0x4322)', 0xA123: 'firmware running (0xa123)'}.get(v, hex(v))
    except Exception as e:
        return f'no answer ({e})'


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else 'status'
    d = sysdev()
    if d is None:
        sys.exit('dongle not found')
    if cmd == 'reset':
        bus, dev = int(open(d + '/busnum').read()), int(open(d + '/devnum').read())
        fd = os.open(f'/dev/bus/usb/{bus:03d}/{dev:03d}', os.O_WRONLY)
        try:
            fcntl.ioctl(fd, USBDEVFS_RESET, 0)
        finally:
            os.close(fd)
        time.sleep(2)
    elif cmd == 'authorize':
        for v in ('0', '1'):
            open(d + '/authorized', 'w').write(v)
            time.sleep(1.5)
    print(f'{os.path.basename(d)}: {status()}')


if __name__ == '__main__':
    main()
