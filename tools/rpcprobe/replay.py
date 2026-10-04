"""Data-driven replay of a captured host call sequence (see tools/seqextract.py).

Every sync reply is compared with the captured one:
  match      identical bytes
  volatile   differs, but the call is known to return changing values (counters, TSF, noise)
  DIVERGE    differs otherwise -> stop (unless keep_going)
  LENGTH     reply length differs -> always stop

Safety rules (docs/protocol.md §8):
  * register access (ids 1 REG_READ, 2 REG_WRITE) is refused until BMAC_INIT (18) has been sent
    and a later sync call has returned (i.e. the dongle came back from init);
  * TX (61 BMAC_TXFIFO) is refused unless allow_tx=True.
"""
import json, struct, time

import rpcids

REG_IDS = {1, 2}
BMAC_INIT, TXFIFO = 18, 61

# Sync calls whose replies legitimately vary between runs. Key: id, or (id, first arg word).
DEFAULT_VOLATILE = {
    45,             # READ_TSF
    153,            # CCA_STATS_READ: channel-time counters [I]
    67, 101,        # tx power readings
    86,             # PHY noise average
    (1, 0x65a),     # REG_READ 0x65a: free-running counter in the capture itself
    (110, 3),       # DOIOVARS iovar 3 (get, 4 B): low bits differ run-to-run, value not reused [I]
}

# Replies whose tail is uninitialised firmware memory: compare only the first N bytes.
# 75 REVINFO_GET: words 23..30 differ run-to-run by scattered bit flips (SRAM noise) [C 2026-10-04].
DEFAULT_PREFIX = {75: 92}


def load(path):
    with open(path, encoding='utf-8') as f:
        lines = [json.loads(l) for l in f if l.strip()]
    return lines[0], lines[1:]


def _key(e):
    a = bytes.fromhex(e['args'])
    return (e['id'], struct.unpack_from('<I', a)[0]) if len(a) >= 4 else e['id']


class Replay:
    def __init__(self, session, path, volatile=None, keep_going=False, allow_tx=False,
                 aggregate=False, log=print):
        self.s, self.log = session, log
        self.header, self.entries = load(path)
        self.volatile = DEFAULT_VOLATILE if volatile is None else volatile
        self.keep_going, self.allow_tx, self.aggregate = keep_going, allow_tx, aggregate
        self.cap_mac = bytes.fromhex(self.header['mac'].replace(':', '')) if self.header.get('mac') else None
        self.live_mac = None
        self.init_sent = False
        self.init_done = False
        self.stats = dict(calls=0, syncs=0, match=0, volatile=0, diverge=0)
        self.divergences = []

    def _subst(self, b):
        if self.cap_mac and self.live_mac and self.cap_mac != self.live_mac:
            b = b.replace(self.cap_mac, self.live_mac)
        return b

    def _guard(self, e):
        if e['id'] in REG_IDS and not self.init_done:
            raise RuntimeError(f'refusing register access (id {e["id"]}) before BMAC_INIT completed')
        if e['id'] == TXFIFO and not self.allow_tx:
            raise RuntimeError('refusing TX (id 61) in a receive-only replay')

    def run(self, stop_after=None):
        """Replay entries; returns True if completed without a stopping divergence.
        stop_after: optional entry index to stop at (for bisecting)."""
        pending_calls = []
        t0 = time.time()
        for i, e in enumerate(self.entries):
            if stop_after is not None and i > stop_after:
                break
            if e['op'] == 'async':
                continue
            self._guard(e)
            args = self._subst(bytes.fromhex(e['args']))
            if e['op'] == 'call':
                if self.aggregate and pending_calls and pending_calls[-1][0] == e['group'] \
                        and len(pending_calls) < 3:
                    pending_calls.append((e['group'], e['id'], args))
                else:
                    self._flush(pending_calls)
                    pending_calls = [(e['group'], e['id'], args)]
                if not self.aggregate:
                    self._flush(pending_calls)
                    pending_calls = []
                if e['id'] == BMAC_INIT:
                    self.init_sent = True
                self.stats['calls'] += 1
                continue
            self._flush(pending_calls)
            pending_calls = []
            reply = self.s.sync(e['id'], args, timeout=3000)
            self.stats['syncs'] += 1
            if self.init_sent:
                self.init_done = True
            if e['id'] == 56:
                self.live_mac = bytes(reply[:6])
            if not self._compare(i, e, reply):
                return False
        self._flush(pending_calls)
        self.log(f'replay done in {time.time() - t0:.1f} s: {self.stats}')
        return True

    def _flush(self, pending):
        if not pending:
            return
        if len(pending) == 1:
            self.s.call(pending[0][1], pending[0][2])
        else:
            self.s.call_many([(i, a) for _, i, a in pending])

    def _compare(self, i, e, reply):
        expect = self._subst(bytes.fromhex(e.get('expect', '')))
        name = rpcids.name(e['id']) or '?'
        if reply == expect:
            self.stats['match'] += 1
            return True
        n = DEFAULT_PREFIX.get(e['id'])
        if n and len(reply) == len(expect) and reply[:n] == expect[:n]:
            self.stats['match'] += 1
            return True
        if len(reply) != len(expect):
            self.log(f'[{i}] LENGTH  id {e["id"]} {name}: got {len(reply)} B, capture {len(expect)} B')
            self.divergences.append((i, e['id'], 'length', reply.hex(), expect.hex()))
            return False
        k = _key(e)
        if e['id'] in self.volatile or k in self.volatile:
            self.stats['volatile'] += 1
            return True
        self.stats['diverge'] += 1
        self.divergences.append((i, e['id'], 'value', reply.hex(), expect.hex()))
        self.log(f'[{i}] DIVERGE id {e["id"]} {name} args {e["args"][:32]}: got {reply[:32].hex()} '
                 f'capture {expect[:32].hex()}')
        return self.keep_going
