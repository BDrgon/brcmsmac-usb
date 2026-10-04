#!/usr/bin/env python3
"""WLRPC id <-> name mapping for BMAC firmware 5.100.148.5.

The names come from the 5.75.11 enum (tools/data/wlrpc_5_75.txt, ISC). Firmware 5.100 renumbers:
ids <= 61 are unchanged; ids 65-142 are the 5.75 index + 1 (confirmed up to 142); 62-64 are
unmapped (never observed). Above 142 new ids were inserted, so the shift grows: the ids we have
evidence for are listed in OVERRIDES_5_100 (docs/protocol.md §5); others above 142 are unmapped.
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

# 5.100 wire ids above 142, assigned by behaviour (docs/protocol.md §5). None = id seen, name unknown.
OVERRIDES_5_100 = {
    153: 'WLRPC_WLC_BMAC_CCA_STATS_READ_ID',   # 5.75 #150 (+3), periodic 28-byte counter poll [I]
    154: 'WLRPC_WLC_BMAC_ANTSEL_SET_ID',       # 5.75 #151 (+3), once at attach with 0 [I]
    157: 'WLRPC_WLC_PHY_LDPC_SET_ID',          # 5.75 #153 (+4), after TXANT_SET in each bring-up [I]
    161: None,                                 # new in 5.100: sync, returns 1 [?]
}
CONFIRMED_SHIFT_MAX = 142


def name(rpc_id, fw='5.100'):
    """5.100 wire id -> WLRPC_* name (or None)."""
    if fw != '5.100':
        return ENUM_5_75.get(rpc_id)
    if rpc_id <= 61:
        return ENUM_5_75.get(rpc_id)
    if rpc_id in OVERRIDES_5_100:
        return OVERRIDES_5_100[rpc_id]
    if 65 <= rpc_id <= CONFIRMED_SHIFT_MAX:
        return ENUM_5_75.get(rpc_id - 1)
    return None


def wire_id(nm, fw='5.100'):
    """WLRPC_* name (with or without the WLRPC_ prefix / _ID suffix) -> 5.100 wire id."""
    def match(n):
        return n and nm in (n, n.removeprefix('WLRPC_'), n.removeprefix('WLRPC_').removesuffix('_ID'))
    if fw == '5.100':
        for i, n in OVERRIDES_5_100.items():
            if match(n):
                return i
    for i, n in ENUM_5_75.items():
        if match(n):
            if fw != '5.100' or i <= 61:
                return i
            return i + 1 if 64 <= i < CONFIRMED_SHIFT_MAX else None
    return None


if __name__ == '__main__':
    args = sys.argv[1:] or [str(i) for i in range(0, 162)]
    for a in args:
        if a.isdigit():
            print(f'{int(a):3d}  {name(int(a)) or "?"}')
        else:
            print(f'{wire_id(a)}  {a}')
