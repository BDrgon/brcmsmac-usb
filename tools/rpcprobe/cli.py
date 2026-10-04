"""rpcprobe command line (run as root on the target, from the tools/ directory):

  python3 -m rpcprobe info                         DL_GETVER / RDL state (no session)
  python3 -m rpcprobe probe   [--fw TRX]           connect + read-only identity calls
  python3 -m rpcprobe up      --seq SEQ [--keep-going] [--aggregate]
                                                   connect + replay a captured init sequence
  python3 -m rpcprobe rx      --seq SEQ --chanblocks BLK --chan 6 [--chan 1 ...] [--seconds 15]
                              [--no-up] [--out FILE.json]
                                                   up (unless --no-up), retune per channel, list beacons
  python3 -m rpcprobe tx      --seq SEQ --chanblocks BLK --chan 6 [--ssid-hex HEX] [--bcast 2]
                              [--directed 2] [--listen 4] [--no-up] [--out FILE.json]
                                                   tune, listen, send a few probe requests (CCK 1 Mbit/s),
                                                   report tx status and probe responses to our MAC
  python3 -m rpcprobe sync ID [HEXARGS]            one raw sync call on a connected session
  python3 -m rpcprobe call ID [HEXARGS]            one raw one-way call

Common: --fw TRX (firmware image if the dongle is in the boot ROM), --trace FILE.jsonl,
--session N (default 0x69). Sequence / block files come from tools/seqextract.py.
Output can contain real MACs/SSIDs; traces are written mode 0600, keep them out of git.
"""
import argparse, collections, glob, json, os, struct, sys, time

import rpcids

from . import rx, tx, xdr
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
    if a.no_up:
        s = open_session(a)
        if not s.reconnected:
            s.close()
            sys.exit('--no-up needs firmware that was already brought up (run `up` first)')
    else:
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
        if not a.no_bcns_promisc:
            # MCTRL(mask, value): MCTL_BCNS_PROMISC (0x00100000, brcmsmac d11.h) = accept beacons
            # from any BSS. The vendor driver's last scan block clears it again (end of scan).
            s.call(42, xdr.u32(0x00100000, 0x00100000))
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


def cmd_tx(a):
    """Phase 2.4/2.5: tune, listen, send a few probe requests, report tx status + probe responses."""
    if a.no_up:
        s = open_session(a)
        if not s.reconnected:
            s.close()
            sys.exit('--no-up needs firmware that was already brought up (run `up` first)')
    else:
        s, ok = cmd_up(a, keep_open=True)
        if not ok:
            s.close()
            sys.exit('init replay did not complete; not transmitting')
    me = xdr.Reader(s.sync(56)).mac()
    me_b = tx.mac(me)
    blocks = json.load(open(a.chanblocks))
    blk = blocks.get(str(a.chan))
    if not blk:
        s.close()
        sys.exit(f'no retune block for channel {a.chan}')
    target_ssid = bytes.fromhex(a.ssid_hex) if a.ssid_hex else None
    bss = {}
    to_us = []
    statuses = []

    def on_recv(m):
        try:
            f = rx.parse_recv(m.args)
        except ValueError:
            return
        if not f.fcs_ok:
            return
        info = rx.beacon_info(f)
        if info:
            e = bss.setdefault(info['bssid'], dict(info, rssi=-127, n=0, ssid_raw=b''))
            e['n'] += 1
            e['rssi'] = max(e['rssi'], f.rssi)
            for eid, v in rx.ies(f.frame):
                if eid == 0:
                    e['ssid_raw'] = v
                    break
        if f.ftype == rx.FT_MGMT and f.frame[4:10] == me_b:
            to_us.append(dict(t=time.time(), subtype=f.subtype, ta=f.addr(2), rssi=f.rssi,
                              ssid=(info or {}).get('ssid')))

    def on_status(m):
        d = tx.parse_txstatus(m.args)
        d['t'] = time.time()
        statuses.append(d)

    s.on(81, on_recv)
    s.on(82, on_status)
    diffs = run_block(s, blk)
    s.call(42, xdr.u32(0x00100000, 0x00100000))     # MCTL_BCNS_PROMISC on (see protocol §7.3)
    print(f'tuned to channel {a.chan}; our MAC {me}' + (f' (block divergences: {diffs})' if diffs else ''))
    time.sleep(a.listen)

    target = None
    cands = [(b, e) for b, e in bss.items() if e.get('ds_channel') == a.chan]
    if target_ssid is not None:
        cands = [(b, e) for b, e in cands if e['ssid_raw'] == target_ssid]
    if cands:
        target = max(cands, key=lambda be: be[1]['rssi'])
    print(f'{len(bss)} BSSs heard; directed target: '
          + (f'{target[0]} rssi {target[1]["rssi"]} ssid {target[1]["ssid"]!r}' if target else 'none'))

    sent = []
    seq = [0x100]

    def send(da, bssid, ssid, label):
        # Pre-TX steps exactly as the vendor driver does before each scan probe (protocol §7.1).
        s.call(42, xdr.u32(0x00100000, 0x00100000))
        s.call(51, xdr.u32(0x5a, 0x3f))
        s.sync(45)
        args, fid, frame = tx.build_probe(me_b, a.chan, seq[0], ssid=ssid, da=da, bssid=bssid)
        seq[0] += 1
        t = time.time()
        s.call(61, args)
        sent.append(dict(t=t, fid=fid, label=label, da=tx.mac(da).hex(':'), len=len(frame)))
        print(f'{time.strftime("%H:%M:%S", time.localtime(t))}.{int(t % 1 * 1000):03d} '
              f'TX {label} fid 0x{fid:04x} {len(frame)} B')
        time.sleep(a.gap)

    for _ in range(a.bcast):
        send(tx.BROADCAST, tx.BROADCAST, b'', 'probe-req wildcard/broadcast')
    if target:
        for _ in range(a.directed):
            send(tx.mac(target[0]), tx.mac(target[0]), target[1]['ssid_raw'], 'probe-req directed/unicast')
    time.sleep(1.0)
    s.close()

    by_fid = {d['frameid']: d for d in statuses}
    print('\ntx status:')
    ok_status = True
    for e in sent:
        d = by_fid.get(e['fid'])
        if not d:
            ok_status = False
            print(f'  fid 0x{e["fid"]:04x} {e["label"]}: NO STATUS')
            continue
        print(f'  fid 0x{e["fid"]:04x} {e["label"]}: status 0x{d["status"]:04x} valid={d["valid"]} '
              f'ack={d["ack"]} tx_count={d["frm_rtx"]} supr={d["supr"]} seq={d["sequence"]} '
              f'(+{(d["t"] - e["t"]) * 1000:.0f} ms)')
    print(f'\nmanagement frames addressed to us: {len(to_us)}')
    for r in to_us:
        print(f'  subtype {r["subtype"]} from {r["ta"]} rssi {r["rssi"]} ssid {r["ssid"]!r}')
    pr = [r for r in to_us if r['subtype'] == rx.ST_PROBE_RESP]
    acked = any(by_fid.get(e['fid'], {}).get('ack') for e in sent if 'unicast' in e['label'])
    print(f'\nG2b: probe responses to our MAC: {len(pr)}; directed probe ACKed: {acked}; '
          f'all tx statuses received: {ok_status}')
    if a.out:
        fd = os.open(a.out, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(fd, 'w') as f:
            json.dump(dict(sent=sent, statuses=statuses, to_us=to_us,
                           target=target[0] if target else None), f, default=str)


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
            p.add_argument('--no-bcns-promisc', action='store_true',
                           help='do not force beacon-promiscuous mode after each retune')
            p.add_argument('--no-up', action='store_true',
                           help='skip the init replay (dongle already up from a previous `up`)')
    p = sub.add_parser('tx')
    p.add_argument('--seq', required=True)
    p.add_argument('--chanblocks', required=True)
    p.add_argument('--chan', type=int, required=True, choices=range(1, 12),
                   help='2.4 GHz channel 1-11 only')
    p.add_argument('--listen', type=float, default=4, help='seconds to listen before transmitting')
    p.add_argument('--ssid-hex', help='directed-probe target SSID (hex; avoids shell quoting)')
    p.add_argument('--bcast', type=int, default=2, choices=range(0, 6), help='wildcard broadcast probes')
    p.add_argument('--directed', type=int, default=2, choices=range(0, 6), help='unicast directed probes')
    p.add_argument('--gap', type=float, default=0.5, help='seconds between frames')
    p.add_argument('--out')
    p.add_argument('--no-up', action='store_true')
    p.add_argument('--keep-going', action='store_true')
    p.add_argument('--aggregate', action='store_true')
    for name in ('sync', 'call'):
        p = sub.add_parser(name)
        p.add_argument('id', type=int)
        p.add_argument('hexargs', nargs='?')
    a = ap.parse_args(argv)
    try:
        {'info': cmd_info, 'probe': cmd_probe, 'up': cmd_up, 'rx': cmd_rx, 'tx': cmd_tx,
         'sync': lambda a: cmd_raw(a, True), 'call': lambda a: cmd_raw(a, False)}[a.cmd](a)
    except Disconnected as e:
        sys.exit(f'DISCONNECTED: {e} (firmware self-reboot or unplug; rerun to re-download)')
    except DongleError as e:
        sys.exit(f'ERROR: {e}')


if __name__ == '__main__':
    main()
