"""Transport framing for the BCM4322 USB BMAC RPC link (docs/protocol.md §3-4).

Pure functions, no USB access:
    frame on the wire  = u32le tp_len | tp_len bytes of RPC message
    RPC message        = u32le hdr (kind<<24 | session<<16 | seq) | body
    body of CALL/SYNC  = u32le rpc_id | args...
"""
import struct
from dataclasses import dataclass, field

CALL, SYNC, MGMT = 1, 2, 3
KIND_NAMES = {CALL: 'CALL', SYNC: 'SYNC', MGMT: 'MGMT'}

# Limits observed on the wire / in the ISC bcm_rpc_tp.h (AGG_MAX_SFRAME = 3).
AGG_MAX_FRAMES = 3


class FramingError(ValueError):
    pass


@dataclass
class Message:
    kind: int
    session: int
    seq: int
    body: bytes = b''
    meta: dict = field(default_factory=dict)

    @property
    def rpc_id(self):
        """First body word: rpc_id for CALL/SYNC, action for MGMT."""
        return struct.unpack_from('<I', self.body)[0] if len(self.body) >= 4 else None

    @property
    def args(self):
        return self.body[4:]

    def words(self):
        n = len(self.body) // 4
        return list(struct.unpack_from(f'<{n}I', self.body)) if n else []


def encode_message(kind, session, seq, body):
    hdr = (kind & 0xff) << 24 | (session & 0xff) << 16 | (seq & 0xffff)
    return struct.pack('<I', hdr) + bytes(body)


def decode_message(raw):
    if len(raw) < 4:
        raise FramingError(f'message too short ({len(raw)} B)')
    hdr = struct.unpack_from('<I', raw)[0]
    return Message(hdr >> 24, (hdr >> 16) & 0xff, hdr & 0xffff, bytes(raw[4:]))


def frame(msg_bytes):
    """Wrap one RPC message in a transport frame."""
    return struct.pack('<I', len(msg_bytes)) + bytes(msg_bytes)


def aggregate(msgs):
    """Concatenate up to AGG_MAX_FRAMES framed messages into one USB transfer."""
    if not 1 <= len(msgs) <= AGG_MAX_FRAMES:
        raise ValueError(f'aggregate takes 1..{AGG_MAX_FRAMES} messages')
    return b''.join(frame(m) for m in msgs)


def deframe(buf):
    """Split one USB transfer into raw RPC messages (handles aggregation).
    Raises FramingError on a truncated or zero-length frame."""
    out, off = [], 0
    while off < len(buf):
        if off + 4 > len(buf):
            raise FramingError(f'trailing {len(buf) - off} B at offset {off}')
        n = struct.unpack_from('<I', buf, off)[0]
        if n < 4 or off + 4 + n > len(buf):
            raise FramingError(f'bad tp_len {n} at offset {off} (transfer {len(buf)} B)')
        out.append(bytes(buf[off + 4:off + 4 + n]))
        off += 4 + n
    return out
