"""USB device access and RPC session for the BCM4322 USB BMAC dongle (needs pyusb + root).

    dev = Device.open()
    s = Session(dev, trace=Trace('work/trace.jsonl'))
    s.start('bcm4322-bmac-5.100.148.5.trx')      # download if in boot ROM, HELLO, CONNECT, reader
    rev = s.sync(75)                              # BMAC_REVINFO_GET -> reply body after echoed id
    s.call(41)                                    # ENABLE_MAC (one-way)
    s.on(81, handler)                             # async WLC_RECV frames from the dongle
    s.close()

See docs/protocol.md for the wire format and docs/recovery.md for failure modes. RAM-only:
nothing persists on the dongle across a replug.
"""
import collections, errno, struct, threading, time

import usb.core
import usb.util

from . import transport
from .transport import CALL, SYNC, MGMT, Message

VID, PID = 0x0846, 0x9020
EP_OUT, EP_IN = 0x03, 0x82
REQ_IN = 0xC1                         # vendor | interface | device-to-host
DL_GETSTATE, DL_GO, DL_START, DL_GETVER, DL_RESETCFG, RPC_RETURN = 0, 2, 3, 5, 8, 9
RETURN_WLEN = 1928                    # wLength the vendor driver uses for sync replies
CHIP_ROM, CHIP_RUNNING = 0x4322, 0xA123
RDL_CHUNK = 1500
RDL_STATES = {0: 'WAITING', 1: 'READY', 2: 'BAD_HDR', 3: 'BAD_CRC', 4: 'RUNNABLE',
              5: 'START_FAIL', 6: 'NVRAM_TOOBIG', 7: 'IMAGE_TOOBIG'}

# Firmware version word required by CONNECT: major<<24 | minor<<16 | build<<8 | patch.
FIRMWARE = {
    '5.100.148.5': 0x05649405,
    '5.60.180.11': 0x053CB40B,
}

MGMT_HELLO, MGMT_CONNECT, MGMT_ACCEPT, MGMT_REFUSE = 1, 2, 3, 5


class DongleError(RuntimeError):
    pass


class Disconnected(DongleError):
    """The device left the bus (self-reboot to boot ROM, unplug) or stopped answering."""


class Device:
    """Thin pyusb wrapper: vendor requests, bulk endpoints, firmware download."""

    def __init__(self, udev):
        self.udev = udev
        try:
            if udev.is_kernel_driver_active(0):
                raise DongleError('a kernel driver is bound to interface 0; unbind it first')
        except (NotImplementedError, usb.core.USBError):
            pass
        try:
            udev.set_configuration()
        except usb.core.USBError as e:
            if e.errno != errno.EBUSY:
                raise
        usb.util.claim_interface(udev, 0)

    @classmethod
    def open(cls):
        udev = usb.core.find(idVendor=VID, idProduct=PID)
        if udev is None:
            raise Disconnected('device 0846:9020 not found')
        return cls(udev)

    def close(self):
        try:
            usb.util.release_interface(self.udev, 0)
        except usb.core.USBError:
            pass
        usb.util.dispose_resources(self.udev)

    # vendor requests --------------------------------------------------------------------------
    def vreq(self, req, length=8, timeout=1000):
        try:
            return bytes(self.udev.ctrl_transfer(REQ_IN, req, 0, 0, length, timeout=timeout))
        except usb.core.USBError as e:
            raise _usb_err(e, f'vendor request {req}')

    def getver(self):
        return struct.unpack_from('<I', self.vreq(DL_GETVER))[0]

    def rdl_state(self):
        return struct.unpack('<II', self.vreq(DL_GETSTATE)[:8])

    def download(self, image, log=print):
        """Download a TRX image to the boot ROM and start it (DL_GO)."""
        n = len(image)
        st, got = struct.unpack('<II', self.vreq(DL_START)[:8])
        if st != 0:
            raise DongleError(f'boot ROM not WAITING (state {RDL_STATES.get(st, st)}); replug')
        sent, t0 = 0, time.time()
        while got != n:
            if got == sent and sent < n:
                self.bulk_out(image[sent:sent + RDL_CHUNK], timeout=5000)
                sent = min(n, sent + RDL_CHUNK)
            st, got = self.rdl_state()
            if st in (2, 3, 5, 6, 7):
                raise DongleError(f'download failed: {RDL_STATES[st]} at {got}/{n} B')
            if time.time() - t0 > 20:
                raise DongleError(f'download stalled at {got}/{n} B')
        for _ in range(50):
            st, got = self.rdl_state()
            if st == 4:
                break
            time.sleep(0.1)
        if st != 4:
            raise DongleError(f'image not RUNNABLE (state {RDL_STATES.get(st, st)})')
        log(f'downloaded {n} B in {time.time() - t0:.1f} s; DL_GO')
        try:
            self.vreq(DL_GO)
        except Disconnected:
            pass                   # some ROMs drop off the bus here; caller re-checks DL_GETVER

    # endpoints --------------------------------------------------------------------------------
    def bulk_out(self, data, timeout=2000):
        try:
            self.udev.write(EP_OUT, data, timeout=timeout)
        except usb.core.USBError as e:
            raise _usb_err(e, 'bulk OUT')

    def bulk_in(self, size=16384, timeout=200):
        """Returns bytes, or None on timeout."""
        try:
            return bytes(self.udev.read(EP_IN, size, timeout=timeout))
        except usb.core.USBTimeoutError:
            return None
        except usb.core.USBError as e:
            raise _usb_err(e, 'bulk IN')

    def rpc_return(self, timeout=2000):
        try:
            return bytes(self.udev.ctrl_transfer(REQ_IN, RPC_RETURN, 0, 0, RETURN_WLEN,
                                                 timeout=timeout))
        except usb.core.USBTimeoutError as e:
            raise DongleError(f'no reply on request 9 within {timeout} ms') from e
        except usb.core.USBError as e:
            raise _usb_err(e, 'request 9')


def _usb_err(e, what):
    if isinstance(e, usb.core.USBTimeoutError):
        return DongleError(f'{what}: timeout')
    if e.errno in (errno.ENODEV, errno.ENOENT, errno.ESHUTDOWN, errno.EPROTO, errno.EPIPE,
                   errno.EIO):
        return Disconnected(f'{what}: {e}')
    return DongleError(f'{what}: {e}')


class Session:
    """One RPC session: handshake, sync/one-way calls, async dispatch."""

    def __init__(self, dev, session=0x69, firmware='5.100.148.5', trace=None, log=print):
        self.dev, self.session, self.log = dev, session, log
        self.version = FIRMWARE[firmware]
        self.trace = trace
        self.chip = None
        self.reconnected = False
        self._seq = 0
        self._lock = threading.Lock()          # serialises bulk OUT + sync replies
        self._handlers = {}
        self.async_counts = collections.Counter()
        self.async_log = collections.deque(maxlen=4096)   # (t, Message) of unhandled frames
        self._reader = None
        self._stop = threading.Event()
        self.error = None                      # set by the reader thread on fatal errors

    # setup ------------------------------------------------------------------------------------
    def start(self, fw_path=None, hello_word3=0, reader=True):
        v = self.dev.getver()
        self._event('getver', chip=hex(v))
        if v == CHIP_ROM:
            if not fw_path:
                raise DongleError('dongle is in the boot ROM and no firmware path was given')
            self.dev.download(open(fw_path, 'rb').read(), log=self.log)
            time.sleep(0.5)
            v = self.dev.getver()
            self._event('getver', chip=hex(v))
        if v != CHIP_RUNNING:
            raise DongleError(f'firmware not running (DL_GETVER 0x{v:x}); replug')
        self.dev.vreq(DL_RESETCFG)
        self._handshake(hello_word3)
        if reader:
            self.start_reader()
        return self

    def _handshake(self, word3):
        r = self._mgmt(MGMT_HELLO, word3)
        w = r.words()
        if w[0] != MGMT_HELLO or len(w) < 4:
            raise DongleError(f'unexpected HELLO reply {w}')
        self.chip = w[3]
        r = self._mgmt(MGMT_CONNECT, self.chip)
        w = r.words()
        if w[0] == MGMT_REFUSE:
            raise DongleError(f'CONNECT refused (status {w[2]}): firmware version mismatch?')
        if w[0] != MGMT_ACCEPT:
            raise DongleError(f'unexpected CONNECT reply {w}')
        self.reconnected = w[2] == 2
        self._event('connected', chip=hex(self.chip), reconnected=self.reconnected)

    def _mgmt(self, action, word3):
        body = struct.pack('<III', action, self.version, word3)
        msg = Message(MGMT, self.session, 0, body)
        with self._lock:
            self._send([msg])
            deadline = time.time() + 2.5
            while time.time() < deadline:
                data = self.dev.bulk_in(timeout=300)
                if not data:
                    continue
                for raw in transport.deframe(data):
                    m = transport.decode_message(raw)
                    self._trace('D>H', 'bulk82', m)
                    if m.kind == MGMT:
                        return m
                    self._dispatch(m)
        raise DongleError(f'no reply to management action {action}')

    # calls ------------------------------------------------------------------------------------
    def call(self, rpc_id, args=b''):
        """One-way call (kind CALL)."""
        with self._lock:
            self._send([self._msg(CALL, rpc_id, args)])

    def call_many(self, items):
        """Up to 3 one-way calls aggregated into a single USB transfer: [(id, args), ...]."""
        with self._lock:
            self._send([self._msg(CALL, i, a) for i, a in items])

    def sync(self, rpc_id, args=b'', timeout=2000):
        """Call expecting a reply; returns the reply body after the echoed rpc_id."""
        self._check()
        with self._lock:
            self._send([self._msg(SYNC, rpc_id, args)])
            data = self.dev.rpc_return(timeout)
        frames = transport.deframe(data)
        if len(frames) != 1:
            raise DongleError(f'request 9 returned {len(frames)} frames')
        m = transport.decode_message(frames[0])
        self._trace('D>H', 'ctl09', m)
        if m.kind != SYNC or m.rpc_id != rpc_id:
            raise DongleError(f'reply mismatch: kind {m.kind} id {m.rpc_id} for call {rpc_id}')
        return m.args

    def _msg(self, kind, rpc_id, args):
        m = Message(kind, self.session, self._seq, struct.pack('<I', rpc_id) + bytes(args))
        self._seq = (self._seq + 1) & 0xffff
        return m

    def _send(self, msgs):
        raws = [transport.encode_message(m.kind, m.session, m.seq, m.body) for m in msgs]
        for m in msgs:
            self._trace('H>D', 'bulk03', m)
        self.dev.bulk_out(transport.aggregate(raws))

    # async ------------------------------------------------------------------------------------
    def on(self, rpc_id, handler):
        """handler(Message) for async frames with this rpc_id (runs in the reader thread)."""
        self._handlers[rpc_id] = handler

    def start_reader(self):
        if self._reader:
            return
        self._stop.clear()
        self._reader = threading.Thread(target=self._read_loop, name='bmac-rx', daemon=True)
        self._reader.start()

    def _read_loop(self):
        while not self._stop.is_set():
            try:
                data = self.dev.bulk_in(timeout=200)
            except DongleError as e:
                self.error = e
                self._event('reader-error', error=str(e))
                return
            if not data:
                continue
            try:
                raws = transport.deframe(data)
            except transport.FramingError as e:
                self._event('framing-error', error=str(e), hex=data[:64].hex())
                continue
            for raw in raws:
                m = transport.decode_message(raw)
                self._trace('D>H', 'bulk82', m)
                self._dispatch(m)

    def _dispatch(self, m):
        rid = m.rpc_id
        self.async_counts[rid] += 1
        h = self._handlers.get(rid)
        if h:
            try:
                h(m)
            except Exception as e:                       # keep the reader alive
                self._event('handler-error', id=rid, error=repr(e))
        else:
            self.async_log.append((time.time(), m))

    def _check(self):
        if self.error:
            raise self.error

    # teardown ---------------------------------------------------------------------------------
    def close(self):
        self._stop.set()
        if self._reader:
            self._reader.join(timeout=1)
            self._reader = None

    # trace helpers ----------------------------------------------------------------------------
    def _trace(self, d, ch, m):
        if self.trace:
            self.trace.frame(d, ch, m)

    def _event(self, what, **kw):
        if self.trace:
            self.trace.event(what, **kw)
