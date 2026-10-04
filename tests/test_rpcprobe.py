"""Unit tests for tools/rpcprobe against sanitised fixtures (no hardware, no pyusb needed).

run: python3 -m unittest discover -s tests -v      (from the repository root)
"""
import json, os, struct, sys, types, unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))

from rpcprobe import transport, xdr, rx  # noqa: E402

FX = json.load(open(os.path.join(ROOT, 'tests', 'fixtures', 'run2_sanitised.json')))


class TestTransport(unittest.TestCase):
    def test_roundtrip(self):
        raw = transport.encode_message(transport.SYNC, 0x69, 0x1234, xdr.u32(75))
        m = transport.decode_message(transport.deframe(transport.frame(raw))[0])
        self.assertEqual((m.kind, m.session, m.seq, m.rpc_id), (2, 0x69, 0x1234, 75))

    def test_aggregate_limit(self):
        with self.assertRaises(ValueError):
            transport.aggregate([b'\0' * 8] * 4)

    def test_deframe_aggregated_fixture(self):
        frames = transport.deframe(bytes.fromhex(FX['agg_transfer']))
        self.assertEqual(len(frames), 3)
        for f in frames:
            m = transport.decode_message(f)
            self.assertEqual((m.kind, m.rpc_id), (transport.CALL, 81))

    def test_deframe_truncated(self):
        good = bytes.fromhex(FX['agg_transfer'])
        with self.assertRaises(transport.FramingError):
            transport.deframe(good[:-1])
        with self.assertRaises(transport.FramingError):
            transport.deframe(b'\0\0\0\0')

    def test_handshake_fixture(self):
        msgs = [transport.decode_message(bytes.fromhex(x['raw'])) for x in FX['mgmt']]
        self.assertTrue(all(m.kind == transport.MGMT for m in msgs))
        hello_reply, accept = msgs[1].words(), msgs[3].words()
        self.assertEqual(hello_reply[0], 1)
        self.assertEqual(hello_reply[1], 0x05649405)
        self.assertEqual(hello_reply[3], 0x4322)
        self.assertEqual(accept[0], 3)


class TestXdr(unittest.TestCase):
    def test_mac_and_buf(self):
        self.assertEqual(xdr.mac('02:00:00:00:00:01'), bytes.fromhex('0200000000010000'))
        self.assertEqual(xdr.buf(b'abc'), bytes.fromhex('03000000616263 00'.replace(' ', '')))
        r = xdr.Reader(xdr.u32(7) + xdr.buf(b'hello') + xdr.mac('02:00:00:00:00:02'))
        self.assertEqual((r.u32(), r.buf(), r.mac(), r.remaining), (7, b'hello', '02:00:00:00:00:02', 0))

    def test_replies(self):
        rev = transport.decode_message(transport.deframe(bytes.fromhex(FX['replies']['75']['reply']))[0])
        self.assertEqual(rev.rpc_id, 75)
        self.assertEqual(struct.unpack_from('<I', rev.args, 24)[0], 0x4322)
        mac = transport.decode_message(transport.deframe(bytes.fromhex(FX['replies']['56']['reply']))[0])
        self.assertEqual(xdr.Reader(mac.args).mac(), '02:00:00:00:00:01')


class TestRx(unittest.TestCase):
    def test_beacons(self):
        self.assertGreaterEqual(len(FX['beacons']), 3)
        for b in FX['beacons']:
            m = transport.decode_message(bytes.fromhex(b))
            f = rx.parse_recv(m.args)
            self.assertTrue(f.fcs_ok)
            self.assertEqual((f.ftype, f.subtype), (rx.FT_MGMT, rx.ST_BEACON))
            info = rx.beacon_info(f)
            self.assertTrue(info['ssid'].startswith('TESTN') or info['ssid'] == '')
            self.assertTrue(info['bssid'].startswith('02:00:00:00:00:'))
            self.assertIn(f.channel, range(1, 15))
            if info['ds_channel']:
                self.assertEqual(info['ds_channel'], f.channel)
            self.assertLess(f.rssi, 0)


class FakeDevice:
    """Stands in for session.Device: records bulk OUT, serves canned request-9 replies."""

    def __init__(self, replies):
        self.out, self.replies = [], list(replies)

    def bulk_out(self, data, timeout=2000):
        self.out.append(bytes(data))

    def rpc_return(self, timeout=2000):
        return self.replies.pop(0)

    def bulk_in(self, size=16384, timeout=200):
        return None


class TestSession(unittest.TestCase):
    def setUp(self):
        # session imports pyusb; provide a stub module if it isn't installed
        if 'usb' not in sys.modules:
            try:
                import usb  # noqa: F401
            except ImportError:
                usb = types.ModuleType('usb')
                usb.core = types.SimpleNamespace(USBError=OSError, USBTimeoutError=TimeoutError,
                                                 find=lambda **k: None)
                usb.util = types.SimpleNamespace()
                sys.modules.update({'usb': usb, 'usb.core': usb.core, 'usb.util': usb.util})
        from rpcprobe import session
        self.session = session

    def test_sync_matches_reply(self):
        reply = bytes.fromhex(FX['replies']['77']['reply'])
        dev = FakeDevice([reply])
        s = self.session.Session(dev, session=0x69)
        args = bytes.fromhex(FX['replies']['77']['call_args'])
        body = s.sync(77, args)
        m = transport.decode_message(transport.deframe(dev.out[0])[0])
        self.assertEqual((m.kind, m.session, m.seq, m.rpc_id), (transport.SYNC, 0x69, 0, 77))
        self.assertEqual(body[:4], transport.decode_message(transport.deframe(reply)[0]).args[:4])

    def test_sync_rejects_wrong_echo(self):
        dev = FakeDevice([bytes.fromhex(FX['replies']['76']['reply'])])
        s = self.session.Session(dev)
        with self.assertRaises(self.session.DongleError):
            s.sync(75)

    def test_seq_increments_across_kinds(self):
        dev = FakeDevice([bytes.fromhex(FX['replies']['76']['reply'])])
        s = self.session.Session(dev)
        s.call(41)
        s.call_many([(3, xdr.u32(1, 2, 3, 4)), (51, xdr.u32(0x5a, 0x3f))])
        s.sync(76)
        seqs = [transport.decode_message(f).seq for out in dev.out for f in transport.deframe(out)]
        self.assertEqual(seqs, [0, 1, 2, 3])
        self.assertEqual(len(transport.deframe(dev.out[1])), 2)     # aggregated transfer


if __name__ == '__main__':
    unittest.main()
