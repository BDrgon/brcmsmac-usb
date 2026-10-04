"""Unit tests for tools/rpcprobe/tx.py (TXFIFO builders, tx status parser). No hardware needed.

Expected byte strings are the vendor driver's own values from a USB capture, with MAC addresses
replaced by placeholders (02:00:00:00:00:01 = dongle, 02:00:00:00:00:02 = AP).
run: python3 -m unittest discover -s tests -v
"""
import os, struct, sys, unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))

from rpcprobe import tx  # noqa: E402

ME, AP = '02:00:00:00:00:01', '02:00:00:00:00:02'

# Vendor d11txh of a 116-byte broadcast probe request on channel 11, TxFrameID 0x0003 (MAC-free).
VENDOR_TXH_BCAST_PROBE = bytes.fromhex(
    '1800000040000000c00002000200000000000a00140b00000000000000000000000000000000ffffffffffff'
    '000000000000000000000a04c003780000000000000000000000000000000000030000000000000000000000'
    '000000000000000000000000000000000000000000000000')
# Vendor d11txh of a 41-byte unicast auth frame on channel 6, TxFrameID 0x4603, RA sanitised.
VENDOR_TXH_UNICAST_AUTH = bytes.fromhex(
    '19000000b0000000c00002000200000000000a00140600000000000000000000000000000000020000000002'
    '000000000000000000000a0468012d003a010000000000000000000000000000034600000000000000000000'
    '000000000000000000000000000000000000000000000000')
# A captured DOTXSTATUS body (32 B): broadcast probe, valid, sent once, no ACK expected.
VENDOR_STATUS_BCAST = bytes.fromhex(
    '000003000110010012cd03000000f49f010000000000000041c1030000000000')
VENDOR_STATUS_UNICAST_ACKED = bytes.fromhex(
    '0400034603101a00c9de73000000f49f1a000000000000004400000008000900')


class TestPlcp(unittest.TestCase):
    def test_cck_1mbps_matches_vendor(self):
        self.assertEqual(tx.plcp_cck(120).hex(), '0a04c0030000')   # 116 B probe + FCS
        self.assertEqual(tx.plcp_cck(45).hex(), '0a0468010000')    # 41 B auth + FCS

    def test_ack_duration_matches_vendor(self):
        self.assertEqual(tx.ack_duration_us(2), 0x013a)

    def test_rejects_ofdm(self):
        with self.assertRaises(ValueError):
            tx.plcp_cck(100, rate500k=12)


class TestD11txh(unittest.TestCase):
    def test_broadcast_probe_matches_vendor(self):
        frame = bytes.fromhex('40000000') + b'\xff' * 6 + tx.mac(ME) + b'\xff' * 6 + b'\0\0'
        frame += b'\0' * (116 - len(frame))
        h = tx.d11txh(frame, tx.plcp_cck(len(frame) + 4), 11, 0x0003, unicast=False)
        self.assertEqual(len(h), tx.D11_TXH_LEN)
        self.assertEqual(h, VENDOR_TXH_BCAST_PROBE)

    def test_unicast_auth_matches_vendor(self):
        frame = struct.pack('<HH', 0x00b0, 0x013a) + tx.mac(AP) + tx.mac(ME) + tx.mac(AP) + b'\0\0'
        frame += b'\0' * (41 - len(frame))
        h = tx.d11txh(frame, tx.plcp_cck(len(frame) + 4), 6, 0x4603, unicast=True, duration=0x013a)
        self.assertEqual(h, VENDOR_TXH_UNICAST_AUTH)


class TestProbe(unittest.TestCase):
    def test_probe_request_layout(self):
        f = tx.probe_request(ME, b'TESTNET', channel=6)
        self.assertEqual(f[:2], b'\x40\x00')
        self.assertEqual(f[4:10], b'\xff' * 6)
        self.assertEqual(f[10:16], tx.mac(ME))
        self.assertEqual(f[24:26], b'\x00\x07')
        self.assertEqual(f[26:33], b'TESTNET')

    def test_build_directed(self):
        args, fid, frame = tx.build_probe(tx.mac(ME), 6, 5, ssid=b'TESTNET', da=AP, bssid=AP)
        fifo, commit, w2, w3, w4, w5, ln = struct.unpack_from('<7I', args)
        self.assertEqual((fifo, commit, w2, w3, w4, w5), (3, 1, 0xffff, 1, 0, 0))
        self.assertEqual(ln, 112 + 6 + len(frame))
        self.assertEqual(len(args) % 4, 0)
        h = args[28:28 + 112]
        self.assertEqual(struct.unpack_from('<H', h)[0] & tx.TXC_IMMEDACK, tx.TXC_IMMEDACK)
        self.assertEqual(h[38:44], tx.mac(AP))
        self.assertEqual(struct.unpack_from('<H', frame, 2)[0], 0x013a)
        self.assertEqual(fid, (5 << 5) | 3)

    def test_build_broadcast_no_ack(self):
        args, _, frame = tx.build_probe(tx.mac(ME), 1, 0)
        h = args[28:28 + 112]
        self.assertEqual(struct.unpack_from('<H', h)[0], tx.TXC_HWSEQ | tx.TXC_STARTMSDU)
        self.assertEqual(struct.unpack_from('<H', h, 0x0a * 2)[0] >> 8, 1)
        self.assertEqual(struct.unpack_from('<H', frame, 2)[0], 0)


class TestTxStatus(unittest.TestCase):
    def test_broadcast(self):
        d = tx.parse_txstatus(VENDOR_STATUS_BCAST)
        self.assertEqual((d['frameid'], d['status'], d['sequence']), (0x0003, 0x1001, 1))
        self.assertTrue(d['valid'])
        self.assertFalse(d['ack'])
        self.assertEqual(d['frm_rtx'], 1)
        self.assertIsNone(d['supr'])

    def test_unicast_acked(self):
        d = tx.parse_txstatus(VENDOR_STATUS_UNICAST_ACKED)
        self.assertEqual((d['frameid'], d['status']), (0x4603, 0x1003))
        self.assertTrue(d['ack'])

    def test_suppressed_badch(self):
        body = bytearray(VENDOR_STATUS_BCAST)
        struct.pack_into('<H', body, 4, 0x0011)
        self.assertEqual(tx.parse_txstatus(bytes(body))['supr'], 'BADCH')


if __name__ == '__main__':
    unittest.main()
