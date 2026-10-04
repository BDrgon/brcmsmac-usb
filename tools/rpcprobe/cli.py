"""rpcprobe command line (run as root on the target, from the tools/ directory):

  python3 -m rpcprobe info                         DL_GETVER / RDL state (no session)
  python3 -m rpcprobe probe   [--fw TRX]           connect + read-only identity calls
  python3 -m rpcprobe up      --seq SEQ [--keep-going] [--aggregate]
                                                   connect + replay a captured init sequence
  python3 -m rpcprobe rx      --seq SEQ --chanblocks BLK --chan 6 [--chan 1 ...] [--seconds 15]
                                                   up, then retune per channel and list beacons
  python3 -m rpcprobe sync ID [HEXARGS]            one raw sync call on a connected session
  python3 -m rpcprobe call ID [HEXARGS]            one raw one-way call

Common: --fw TRX (firmware image if the dongle is in the boot ROM), --trace FILE.jsonl,
--session N (default 0x69). Sequence / block files come from tools/seqextract.py.
Output can contain real MACs/SSIDs; traces are written mode 0600, keep them out of git.
"""
import argparse, collections, glob, json, os, struct, sys, time

import rpcids

from . import rx, xdr
from .replay import Replay
from .session import Device, Session, DongleError, Disconnected
from .trace import Trace

FW_CANDIDATES = ['bcm4322-bmac-5.100.148.5.trx', '4322-bmac-roml-ag.trx',
                 '../bcm4322-bmac-5.100.148.5.trx', '../4322-bmac-roml-ag.trx',
                 '/lib/firmware/brcm/bcm4322-bmac-5.100.148.5.trx']


def find_fw(arg):
    if arg:
        return arg
    for c in FW_CANDIDATES:
        if os.path.exists(c):
            return c
    return None


def open_session(a):
    tr = Trace(a.trace) if a.trace else None
    dev = Device.open()
    s = Session(dev, session=a.session, trace=tr)
    s.start(find_fw(a.fw))
    print(f'connected: chip 0x{s.chip:x}{" (reconnected to running firmware)" if s.reconnected else ""}')
    return s


def cmd_info(a):
    dev = Device.open()
    v = dev.getver()
    st, n = dev.rdl_state()
    print(f'DL_GETVER 0x{v:x} ({"boot ROM" if v == 0x4322 else "firmware running" if v == 0xA123 else "?"}), '
          f'RDL state {st} bytes {n}')


def cmd_probe(a):
    s = open_session(a)
    rev = xdr.Reader(s.sync(75))
    words = [rev.u32() for _ in range(8)]
    print(f'revinfo: vendor 0x{words[0]:04x} device 0x{words[1]:04x} chip 0x{words[6]:04x}')
    st = s.sync(76)
    print(f'state: {st.hex()}')
    print('tx fifo sizes:', [struct.unpack_from('<I', s.sync(77, xdr.u32(f)))[0] for f in range(6)])
    nv = xdr.Reader(s.sync(91)).buf().split(b'\0')
    keys = [kv.split(b'=', 1)[0].decode() for kv in nv if b'=' in kv]
    print(f'nvram: {len(keys)} vars ({", ".join(keys[:8])} ...)')
    print('mac:', xdr.Reader(s.sync(56)).mac())
    s.close()


def cmd_up(a, keep_open=False):
    s = open_session(a)
    if s.reconnected:
        print('note: firmware was already running; replaying init on a live dongle may diverge')
    r = Replay(s, a.seq, keep_going=a.keep_going, aggregate=a.aggregate)
    ok = r.run()
    print(f'init replay: {"OK" if ok else "STOPPED"} {r.stats}')
    time.sleep(0.3)
    print('async frames during replay:', {f'{k} {rpcids.name(k) or "?"}': v
                                          for k, v in sorted(s.async_counts.items())})
    if keep_open:
        return s, ok
    s.close()
    return None, ok


def run_block(s, block):
    """Replay one retune block (from --chanblocks); returns list of (id, divergence)."""
    diffs = []
    for e in block:
        args = bytes.fromhex(e['args'])
        if e['id'] == 61:
            raise RuntimeError('TX in receive-only block')
        if e['op'] == 'call':
            s.call(e['id'], args)
        else:
            reply = s.sync(e['id'], args, timeout=3000)
            if e['id'] not in (45, 153, 67, 101, 68) and reply.hex() != e.get('expect'):
                diffs.append((e['id'], reply[:16].hex(), e.get('expect', '')[:32]))
    return diffs


def cmd_rx(a):
    s, ok = cmd_up(a, keep_open=True)
    if not ok:
        s.close()
        sys.exit('init replay did not complete; not starting receive')
    blocks = json.load(open(a.chanblocks))
    seen = {}
    counts = collections.Counter()
    cur = {'ch': None}

    def on_recv(m):
        try:
            f = rx.parse_recv(m.args)
        except ValueError:
            counts['bad'] += 1
            return
        counts['frames'] += 1
        counts['fcs_ok' if f.fcs_ok else 'fcs_bad'] += 1
        info = rx.beacon_info(f)
        if info and f.fcs_ok:
            k = info['bssid']
            e = seen.setdefault(k, dict(info, rssi=[], tuned=set(), rxchan=set(), n=0))
            e['n'] += 1
            e['rssi'].append(f.rssi)
            e['tuned'].add(cur['ch'])
            e['rxchan'].add(f.channel)

    s.on(81, on_recv)
    for ch in a.chan:
        blk = blocks.get(str(ch))
        if not blk:
            print(f'no retune block for channel {ch} in {a.chanblocks}')
            continue
        diffs = run_block(s, blk)
        cur['ch'] = ch
        print(f'tuned to channel {ch}' + (f' (block divergences: {diffs})' if diffs else ''))
        time.sleep(a.seconds)
    s.close()
    print(f'frames: {dict(counts)}; async: {dict(s.async_counts)}')
    rows = sorted(seen.values(), key=lambda e: -max(e['rssi']))
    for e in rows:
        print(f'{e["bssid"]}  ch {e["ds_channel"]!s:>2} (rx {sorted(e["rxchan"])}, tuned {sorted(e["tuned"])})  '
              f'rssi {max(e["rssi"]):4d}  n {e["n"]:3d}  {e["ssid"]!r}')
    if a.out:
        fd = os.open(a.out, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(fd, 'w') as f:
            json.dump([dict(e, tuned=sorted(e['tuned']), rxchan=sorted(e['rxchan'])) for e in rows], f)
    print(f'{len(rows)} BSSs seen')


def cmd_raw(a, sync):
    s = open_session(a)
    args = bytes.fromhex(a.hexargs) if a.hexargs else b''
    if sync:
        print(s.sync(a.id, args).hex())
    else:
        s.call(a.id, args)
    s.close()


def main(argv=None):
    ap = argparse.ArgumentParser(prog='rpcprobe')
    ap.add_argument('--fw')
    ap.add_argument('--trace')
    ap.add_argument('--session', type=lambda x: int(x, 0), default=0x69)
    sub = ap.add_subparsers(dest='cmd', required=True)
    sub.add_parser('info')
    sub.add_parser('probe')
    for name in ('up', 'rx'):
        p = sub.add_parser(name)
        p.add_argument('--seq', required=True)
        p.add_argument('--keep-going', action='store_true')
        p.add_argument('--aggregate', action='store_true')
        if name == 'rx':
            p.add_argument('--chanblocks', required=True)
            p.add_argument('--chan', type=int, action='append', required=True)
            p.add_argument('--seconds', type=float, default=15)
            p.add_argument('--out')
    for name in ('sync', 'call'):
        p = sub.add_parser(name)
        p.add_argument('id', type=int)
        p.add_argument('hexargs', nargs='?')
    a = ap.parse_args(argv)
    try:
        {'info': cmd_info, 'probe': cmd_probe, 'up': cmd_up, 'rx': cmd_rx,
         'sync': lambda a: cmd_raw(a, True), 'call': lambda a: cmd_raw(a, False)}[a.cmd](a)
    except Disconnected as e:
        sys.exit(f'DISCONNECTED: {e} (firmware self-reboot or unplug; rerun to re-download)')
    except DongleError as e:
        sys.exit(f'ERROR: {e}')


if __name__ == '__main__':
    main()
