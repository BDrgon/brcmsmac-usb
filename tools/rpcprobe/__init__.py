"""rpcprobe — userspace reference implementation of the BCM4322 USB BMAC RPC protocol.

Modules:
  transport  framing, aggregation, message header (pure)
  xdr        argument encoding/decoding helpers (pure)
  rx         WLC_RECV / 802.11 parsing (pure)
  trace      JSONL frame trace
  session    pyusb device access, firmware download, RPC session, async reader
  replay     data-driven replay of a captured call sequence
  cli        `python3 -m rpcprobe ...`

See docs/protocol.md. Pure modules import without pyusb.
"""
import os as _os
import sys as _sys

# tools/ holds rpcids.py etc.; make it importable however the package is loaded.
_tools = _os.path.dirname(_os.path.dirname(_os.path.abspath(__file__)))
if _tools not in _sys.path:
    _sys.path.insert(0, _tools)
