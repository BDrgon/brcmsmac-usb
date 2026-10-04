"""Argument encoding for BMAC RPC calls (docs/protocol.md §4.3).

Arguments are little-endian u32 words. Fixed byte fields (e.g. a MAC) are sent raw, zero-padded to
a multiple of 4. Variable buffers are preceded by an explicit length word, then padded to 4.
"""
import struct


def pad4(b):
    b = bytes(b)
    return b + b'\0' * (-len(b) % 4)


def u32(*vals):
    return b''.join(struct.pack('<I', v & 0xffffffff) for v in vals)


def mac(addr):
    """'aa:bb:cc:dd:ee:ff' or 6 bytes -> 8 bytes (raw + 2 zero pad)."""
    if isinstance(addr, str):
        addr = bytes(int(x, 16) for x in addr.split(':'))
    if len(addr) != 6:
        raise ValueError('MAC must be 6 bytes')
    return pad4(addr)


def buf(data):
    """Length-prefixed variable buffer."""
    return u32(len(data)) + pad4(data)


class Reader:
    """Sequential reader over a reply/argument byte string."""

    def __init__(self, data):
        self.data, self.off = bytes(data), 0

    def u32(self):
        v = struct.unpack_from('<I', self.data, self.off)[0]
        self.off += 4
        return v

    def u64(self):
        lo, hi = self.u32(), self.u32()
        return hi << 32 | lo

    def raw(self, n, padded=True):
        b = self.data[self.off:self.off + n]
        self.off += n + ((-n) % 4 if padded else 0)
        return b

    def buf(self):
        return self.raw(self.u32())

    def mac(self):
        return ':'.join(f'{x:02x}' for x in self.raw(6))

    def rest(self):
        b = self.data[self.off:]
        self.off = len(self.data)
        return b

    @property
    def remaining(self):
        return len(self.data) - self.off
