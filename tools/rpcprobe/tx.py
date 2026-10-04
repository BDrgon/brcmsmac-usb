"""Build BMAC_TXFIFO (id 61) frames and parse DOTXSTATUS (id 82) (docs/protocol.md §7.1, §7.4).

Pure functions, no USB access. Layouts:

    TXFIFO args = u32 fifo | u32 commit | u32 0xffff | u32 w3 | u32 w4 | u32 w5 | u32 len
                  | len bytes: d11txh (112 B) | PLCP (6 B) | 802.11 frame (no FCS)
                  | zero padding to a multiple of 4
    d11txh      = brcmsmac `struct d11txh` (d11.h, ISC); fields below are named after it.

Only CCK 1-11 Mbit/s management frames are supported here (what scans and probes need).
"""
import struct

D11_TXH_LEN = 112
D11_PHY_HDR_LEN = 6

# brcmsmac d11.h: MacTxControlLow bits
TXC_IMMEDACK = 0x0001
TXC_STARTMSDU = 0x0008
TXC_HWSEQ = 0x0010
# PhyTxControlWord: PHY_TXC_ANT_0_1 (auto antenna)
PHY_TXC_ANT_0_1 = 0x00c0
# brcmsmac d11.h: tx FIFOs
TX_AC_BE_FIFO, TX_AC_VO_FIFO = 1, 3
TX_CTL_FIFO = TX_AC_VO_FIFO
# brcmsmac main.h: TxFrameID
TXFID_QUEUE_MASK, TXFID_SEQ_SHIFT, TXFID_SEQ_MASK = 0x0007, 5, 0x7FE0

# tx status bits (brcmsmac d11.h TX_STATUS_*)
TX_STATUS_VALID = 0x0001
TX_STATUS_ACK_RCV = 0x0002
TX_STATUS_SUPR_MASK, TX_STATUS_SUPR_SHIFT = 0x001C, 2
TX_STATUS_AMPDU = 0x0020
TX_STATUS_INTERMEDIATE = 0x0040
TX_STATUS_PMINDCTD = 0x0080
TX_STATUS_RTS_RTX_MASK, TX_STATUS_RTS_RTX_SHIFT = 0x0F00, 8
TX_STATUS_FRM_RTX_MASK, TX_STATUS_FRM_RTX_SHIFT = 0xF000, 12
SUPR_NAMES = {0: None, 1: 'PMQ', 2: 'FLUSH', 3: 'FRAG/TBTT', 4: 'BADCH', 5: 'EXPTIME', 6: 'UF'}

# CCK PLCP SIGNAL values (rate in 100 kbit/s units)
CCK_SIGNAL = {2: 0x0a, 4: 0x14, 11: 0x37, 22: 0x6e}      # key: rate in 500 kbit/s units
SIFS_US, CCK_LONG_PREAMBLE_US = 10, 192
ACK_LEN = 14                                            # bytes, incl. FCS
FCS_LEN = 4

BROADCAST = b'\xff' * 6
DEFAULT_RATES = bytes([0x82, 0x84, 0x8b, 0x96])          # 1, 2, 5.5, 11 Mbit/s (basic)
DEFAULT_EXT_RATES = bytes([0x0c, 0x12, 0x18, 0x24, 0x30, 0x48, 0x60, 0x6c])  # 6..54 Mbit/s


def mac(s):
    """'aa:bb:cc:dd:ee:ff' or 6 bytes -> 6 bytes."""
    if isinstance(s, (bytes, bytearray)):
        if len(s) != 6:
            raise ValueError('MAC must be 6 bytes')
        return bytes(s)
    b = bytes.fromhex(s.replace(':', '').replace('-', ''))
    if len(b) != 6:
        raise ValueError(f'bad MAC {s!r}')
    return b


def cck_airtime_us(nbytes, rate500k=2):
    """PSDU airtime in µs (no preamble) for a CCK rate given in 500 kbit/s units."""
    return -(-nbytes * 8 * 2 // rate500k)              # ceil(bits / Mbit/s)


def plcp_cck(psdu_len, rate500k=2):
    """6-byte PLCP header for a CCK frame; psdu_len includes the FCS.
    Byte layout as seen on the wire: SIGNAL, SERVICE (0x04 = locked clocks), LENGTH (µs, LE),
    then two zero bytes (CRC filled in by hardware)."""
    if rate500k not in CCK_SIGNAL:
        raise ValueError('CCK rate must be 1, 2, 5.5 or 11 Mbit/s')
    if rate500k == 22 and (psdu_len * 8 * 2) % rate500k:
        raise NotImplementedError('11 Mbit/s length-extension bit not implemented')
    return struct.pack('<BBHH', CCK_SIGNAL[rate500k], 0x04, cck_airtime_us(psdu_len, rate500k), 0)


def ack_duration_us(rate500k=2):
    """Duration/ID for a unicast frame expecting an ACK at a basic CCK rate (long preamble)."""
    return SIFS_US + CCK_LONG_PREAMBLE_US + cck_airtime_us(ACK_LEN, rate500k)


def ie(eid, data):
    data = bytes(data)
    if len(data) > 255:
        raise ValueError('IE too long')
    return bytes([eid, len(data)]) + data


def probe_request(sa, ssid=b'', da=BROADCAST, bssid=BROADCAST, channel=None,
                  rates=DEFAULT_RATES, ext_rates=DEFAULT_EXT_RATES, duration=0):
    """802.11 probe request (no FCS). Sequence control is left 0 (hardware assigns it, TXC_HWSEQ)."""
    if isinstance(ssid, str):
        ssid = ssid.encode('utf-8')
    hdr = struct.pack('<HH', 0x0040, duration) + mac(da) + mac(sa) + mac(bssid) + b'\0\0'
    body = ie(0, ssid) + ie(1, rates)
    if ext_rates:
        body += ie(50, ext_rates)
    if channel is not None:
        body += ie(3, bytes([channel]))
    return hdr + body


def frame_id(seq, fifo):
    return ((seq << TXFID_SEQ_SHIFT) & TXFID_SEQ_MASK) | (fifo & TXFID_QUEUE_MASK)


def d11txh(frame, plcp, channel, fid, unicast, duration=0):
    """112-byte d11txh for a CCK management frame, mirroring what the vendor driver sends.
    frame: 802.11 frame without FCS; plcp: its 6-byte PLCP header."""
    fc = struct.unpack_from('<H', frame)[0]
    h = bytearray(D11_TXH_LEN)
    w = lambda i, v: struct.pack_into('<H', h, i * 2, v)
    w(0x00, TXC_HWSEQ | TXC_STARTMSDU | (TXC_IMMEDACK if unicast else 0))   # MacTxControlLow
    w(0x01, 0)                                     # MacTxControlHigh
    w(0x02, fc)                                    # MacFrameControl
    w(0x04, PHY_TXC_ANT_0_1)                       # PhyTxControlWord
    w(0x05, 0x0002)                                # PhyTxControlWord_1 (20 MHz, as captured)
    w(0x06, 0x0002)                                # PhyTxControlWord_1_Fbr
    w(0x09, plcp[0])                               # MainRates: main/fallback CCK SIGNAL
    w(0x0a, 0x0014 | (channel << 8))               # XtraFrameTypes: frame types | channel << 8
    h[0x13 * 2:0x13 * 2 + 6] = frame[4:10]         # TxFrameRA = addr1
    fb = bytearray(plcp)                           # FragPLCPFallback: PLCP with PSDU length in [4:6]
    struct.pack_into('<H', fb, 4, len(frame) + FCS_LEN)
    h[0x1b * 2:0x1b * 2 + 6] = fb
    w(0x1e, duration)                              # FragDurFallback
    w(0x26, fid)                                   # TxFrameID
    return bytes(h)


def txfifo_args(payload, fifo=TX_CTL_FIFO, commit=1, words=(0xffff, 1, 0, 0)):
    """Arguments of BMAC_TXFIFO (after the rpc_id): 6 words, length, payload, pad to 4."""
    pad = (-len(payload)) % 4
    return struct.pack('<6I', fifo, commit, *words) + struct.pack('<I', len(payload)) + payload + b'\0' * pad


def build_probe(sa, channel, fid_seq, ssid=b'', da=BROADCAST, bssid=BROADCAST, rate500k=2,
                fifo=TX_CTL_FIFO, include_ds=True):
    """Complete TXFIFO argument block for one probe request. Returns (args, frame_id, frame)."""
    unicast = mac(da) != BROADCAST
    dur = ack_duration_us(rate500k) if unicast else 0
    frame = probe_request(sa, ssid, da, bssid, channel if include_ds else None, duration=dur)
    plcp = plcp_cck(len(frame) + FCS_LEN, rate500k)
    fid = frame_id(fid_seq, fifo)
    payload = d11txh(frame, plcp, channel, fid, unicast, dur) + plcp + frame
    return txfifo_args(payload, fifo), fid, frame


def parse_txstatus(args):
    """DOTXSTATUS (id 82) body after the rpc_id: 32 bytes. Field meanings [C] for frameid/status/
    sequence, [I] for the rest (docs/protocol.md §7.4)."""
    if len(args) < 16:
        raise ValueError(f'tx status too short ({len(args)} B)')
    u = struct.unpack_from('<4H', args)
    st = u[2]
    d = dict(word0=u[0], frameid=u[1], status=st, sequence=u[3],
             valid=bool(st & TX_STATUS_VALID), ack=bool(st & TX_STATUS_ACK_RCV),
             ampdu=bool(st & TX_STATUS_AMPDU),
             supr=SUPR_NAMES.get((st & TX_STATUS_SUPR_MASK) >> TX_STATUS_SUPR_SHIFT, '?'),
             frm_rtx=(st & TX_STATUS_FRM_RTX_MASK) >> TX_STATUS_FRM_RTX_SHIFT,
             rts_rtx=(st & TX_STATUS_RTS_RTX_MASK) >> TX_STATUS_RTS_RTX_SHIFT)
    if len(args) >= 12:
        d['tx_tsf_l'] = struct.unpack_from('<I', args, 8)[0]
    if len(args) >= 32:
        d['host_tsf'] = struct.unpack_from('<Q', args, 24)[0]
    return d
