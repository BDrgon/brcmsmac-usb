#!/usr/bin/env python3
"""WLRPC id <-> name mapping for BMAC firmware 5.100.148.5.

The names come from the 5.75.11 enum (tools/data/wlrpc_5_75.txt, ISC). Firmware 5.100 renumbers:
ids <= 61 are unchanged, ids >= 65 are the 5.75 index + 1; 62-64 are unmapped (never observed).
Async dongle->host calls (e.g. 81 WLC_RECV) use the same numbering.

usage: rpcids.py [ID|NAME ...]     # print the mapping (all ids if no argument)
"""
import os, sys

_DATA = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'data', 'wlrpc_5_75.txt')


def _load():
    t = {}
    for line in open(_DATA):
        line = line.strip()
        if line and not line.startswith('#'):
            i, n = line.split(None, 1)
            t[int(i)] = n
    return t


ENUM_5_75 = _load()


def name(rpc_id, fw='5.100'):
    """5.100 wire id -> WLRPC_* name (or None)."""
    if fw != '5.100':
        return ENUM_5_75.get(rpc_id)
    if rpc_id <= 61:
        return ENUM_5_75.get(rpc_id)
    if rpc_id >= 65:
        return ENUM_5_75.get(rpc_id - 1)
    return None


def wire_id(nm, fw='5.100'):
    """WLRPC_* name (with or without the WLRPC_ prefix / _ID suffix) -> 5.100 wire id."""
    for i, n in ENUM_5_75.items():
        if nm in (n, n.removeprefix('WLRPC_'), n.removeprefix('WLRPC_').removesuffix('_ID')):
            if fw != '5.100' or i <= 61:
                return i
            return i + 1 if i >= 64 else None
    return None


if __name__ == '__main__':
    args = sys.argv[1:] or [str(i) for i in range(0, 160)]
    for a in args:
        if a.isdigit():
            print(f'{int(a):3d}  {name(int(a)) or "?"}')
        else:
            print(f'{wire_id(a)}  {a}')
