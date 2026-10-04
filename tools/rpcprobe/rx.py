"""Parse WLC_RECV (id 81) frames from the dongle (docs/protocol.md §7.2).

WLC_RECV body after the rpc_id:
    u32 len | 38 B header | [2 B pad if RxStatus1 & 4] | 6 B PLCP | 802.11 frame incl. FCS
The 38 B header starts with brcmsmac's 24 B `struct d11rxhdr`; RxFrameSize == len - 38 [C].
Bytes 24..27 = low TSF word, byte 28 = RSSI (dBm, signed) [I].
Protected frames arrive already decrypted, so their FCS does not verify.
"""
import struct, zlib
from dataclasses import dataclass

HDR_LEN = 38
RXS_PBPRES = 0x4           # RxStatus1: 2-byte pad present

FT_MGMT, FT_CTRL, FT_DATA = 0, 1, 2
ST_BEACON, ST_PROBE_RESP = 8, 5


@dataclass
class RxFrame:
    size: int
    rxstatus1: int
    rxstatus2: int
    rxtsf: int
    rxchan: int
    tsf_l: int
    rssi: int
    plcp: bytes
    frame: bytes             # 802.11 frame including FCS

    @property
    def channel(self):
        return self.rxchan >> 3

    @property
    def fcs_ok(self):
        f = self.frame
        return len(f) > 4 and struct.unpack('<I', f[-4:])[0] == zlib.crc32(f[:-4]) & 0xffffffff

    @property
    def fc(self):
        return struct.unpack_from('<H', self.frame)[0] if len(self.frame) >= 2 else 0

    @property
    def ftype(self):
        return (self.fc >> 2) & 3

    @property
    def subtype(self):
        return (self.fc >> 4) & 0xf

    def addr(self, n):
        """n = 1..3 -> 'aa:bb:..' (None if the frame is too short)."""
        o = 4 + 6 * (n - 1)
        a = self.frame[o:o + 6]
        return ':'.join(f'{x:02x}' for x in a) if len(a) == 6 else None


def parse_recv(args):
    """args = WLC_RECV body after the rpc_id."""
    ln = struct.unpack_from('<I', args)[0]
    d = args[4:4 + ln]
    if len(d) < HDR_LEN + 6:
        raise ValueError(f'WLC_RECV too short ({len(d)} B)')
    (size, _pad, _p0, _p1, _p2, _p3, _p4, _p5, rs1, rs2, tsf, chan) = struct.unpack_from('<12H', d)
    tsf_l = struct.unpack_from('<I', d, 24)[0]
    rssi = struct.unpack_from('<b', d, 28)[0]
    off = HDR_LEN + (2 if rs1 & RXS_PBPRES else 0)
    return RxFrame(size, rs1, rs2, tsf, chan, tsf_l, rssi, bytes(d[off:off + 6]),
                   bytes(d[off + 6:]))


def ies(frame, fixed=12):
    """Yield (id, bytes) information elements of a beacon/probe response (FCS excluded)."""
    body = frame[24 + fixed:-4]
    o = 0
    while o + 2 <= len(body):
        eid, n = body[o], body[o + 1]
        if o + 2 + n > len(body):
            return
        yield eid, body[o + 2:o + 2 + n]
        o += 2 + n


def beacon_info(rx):
    """For beacons/probe responses: dict(bssid, ssid, ds_channel, interval) else None."""
    if rx.ftype != FT_MGMT or rx.subtype not in (ST_BEACON, ST_PROBE_RESP) or len(rx.frame) < 40:
        return None
    ssid, ds = None, None
    for eid, v in ies(rx.frame):
        if eid == 0 and ssid is None:
            ssid = v.decode('utf-8', errors='replace')
        elif eid == 3 and v:
            ds = v[0]
    interval = struct.unpack_from('<H', rx.frame, 32)[0]
    return dict(bssid=rx.addr(3), ssid=ssid, ds_channel=ds, interval=interval)
