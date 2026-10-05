# Licensing, firmware and interoperability notes

*This page is not legal advice. It records the project's policies and the reasoning behind them.*

## What the project ships and what it doesn't

| Item | Licence / status | Policy |
|---|---|---|
| brcmsmac-derived driver code (`driver/brcmsmac/`, `driver/include/`, `driver/brcmutil/`) | ISC (Broadcom), notices kept | Modified and shipped; module licence `Dual BSD/GPL` |
| New transport code and tools | ISC | Shipped |
| RPC id names (`tools/data/wlrpc_5_75.txt`) | From Linux 2.6.37 staging `wlc_rpc.h`, ISC | Shipped with notice |
| Adapter firmware (inside the vendor's Windows driver) | Broadcom proprietary | **Never shipped.** Users obtain it from their own copy of the vendor package |
| Vendor Windows driver package | Proprietary | **Never shipped** |
| USB captures | Our own traffic, but they contain MACs, network names, possibly key material | **Never shipped raw**; only sanitised fixtures |

## Interoperability

This project writes an independent driver so an adapter the user owns can work with Linux. Protocol
knowledge comes from observing the adapter's USB traffic with the vendor driver, from ISC-licensed
headers, and from experiments on our own hardware. No technical protection measure was circumvented:
the firmware is not encrypted or access-controlled, and the RPC protocol is plain data on the USB bus.

Commonly cited background for interoperability reverse engineering:
- *Sega Enterprises v. Accolade*, 977 F.2d 1510 (9th Cir. 1992), and *Sony Computer Entertainment v.
  Connectix*, 203 F.3d 596 (9th Cir. 2000): intermediate copying and reverse engineering for
  interoperability held to be fair use.
- EU Software Directive (2009/24/EC): Art. 5(3) allows observing and testing a program's
  functioning to understand its underlying ideas. Art. 6 allows decompilation where necessary
  for interoperability, under conditions.
- US DMCA §1201(f) has an interoperability exception for circumventing technical measures. It is
  mentioned for completeness; this project didn't circumvent any.

**Firmware-extraction precedent.** `b43` users have long extracted Broadcom firmware from vendor
driver packages themselves with `b43-fwcutter` (packaged by major distributions), because that
firmware isn't redistributable. The deferred extraction tool (PLAN D8) would follow the same model.
By contrast, brcmsmac's own firmware is in `linux-firmware` under a redistribution licence; that
doesn't apply to this adapter's firmware.

## Clean-room history (an honest account)

The shipped protocol documentation (`docs/protocol.md`) and the driver are based on our USB
captures, the ISC-licensed staging headers and brcmsmac, and hardware experiments.

During early reconnaissance, an analysis step located copies of Broadcom's `bcm_rpc.c`/`bcm_xdr.c`
in public router firmware source trees and read them. Those files carry Broadcom's "unpublished
proprietary source code" notice, not GPL. When this was noticed:
- the files were confined to the private `work/` area and never committed;
- the protocol document was rewritten from capture evidence, and terminology taken from those files
  was replaced with the project's own (CALL/SYNC/MGMT, HELLO/CONNECT/ACCEPT/REFUSE);
- later work was instructed not to open them.

Because the same project (and parts of the same tooling) had seen them, this is **not** a strict
two-team clean room. Before any public release, a reviewer should confirm that no code, structure or
comments derive from those files. Every fact in `docs/protocol.md` should be traceable to capture
evidence, ISC headers or a documented experiment.
