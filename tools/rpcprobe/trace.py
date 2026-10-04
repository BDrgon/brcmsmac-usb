"""Structured trace of every RPC frame, one JSON object per line.

Fields: t (unix time), dir (H>D | D>H), ch (bulk03 | bulk82 | ctl09), kind, session, seq, id,
name, len (body bytes), hex (body, truncated to `maxhex` bytes), plus optional `note`.
Traces contain real device data (MACs, SSIDs, NVRAM): keep them under work/ (gitignored).
"""
import json, os, threading, time

import rpcids                     # tools/ is put on sys.path by rpcprobe/__init__.py

from . import transport


class Trace:
    def __init__(self, path=None, maxhex=256, echo=False):
        self.path, self.maxhex, self.echo = path, maxhex, echo
        self._f = open(path, 'a', encoding='utf-8') if path else None
        self._lock = threading.Lock()
        if path:
            os.chmod(path, 0o600)

    def frame(self, direction, channel, msg, note=None):
        rid = msg.rpc_id
        rec = dict(t=round(time.time(), 6), dir=direction, ch=channel,
                   kind=transport.KIND_NAMES.get(msg.kind, msg.kind), session=msg.session,
                   seq=msg.seq, id=rid, len=len(msg.body),
                   hex=msg.body[:self.maxhex].hex())
        if msg.kind != transport.MGMT and rid is not None:
            rec['name'] = rpcids.name(rid)
        if note:
            rec['note'] = note
        self._write(rec)

    def event(self, what, **kw):
        self._write(dict(t=round(time.time(), 6), event=what, **kw))

    def _write(self, rec):
        line = json.dumps(rec, separators=(',', ':'))
        with self._lock:
            if self._f:
                self._f.write(line + '\n')
                self._f.flush()
            if self.echo:
                print(line)

    def close(self):
        if self._f:
            self._f.close()
            self._f = None
