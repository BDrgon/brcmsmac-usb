# BCM4322 USB BMAC — wire protocol

Independent description of the USB protocol between a host and a Broadcom BCM4322 USB "BMAC"
(split-MAC) dongle running firmware **5.100.148.5**. Reference device: Netgear WNA3100 v1
(`0846:9020`).

**Sources and clean-room rule.** Everything here comes from three places:
1. USB captures of the vendor Windows driver driving hardware we own.
2. Live experiments with our own userspace tools (`tools/`) on that hardware.
3. ISC-licensed Broadcom headers from the Linux v2.6.37 staging tree (`wlc_rpc.h`, `bcm_rpc_tp.h`),
   and the mainline `brcmfmac`/`brcmsmac` drivers.

No proprietary Broadcom source was used (see SPEC §6). Names marked *(ours)* are our own labels
for observed behaviour, not Broadcom's.

**Confidence:** **[C]** confirmed on hardware or across a whole capture · **[I]** inferred from
limited evidence · **[?]** unknown.

**Sanitisation:** MAC addresses are replaced with `02:00:00:00:00:01` (the dongle), `02:00:00:00:00:02`
(an AP) and `02:00:00:00:00:03` (another station). No SSIDs, keys or passphrases appear here.

---

## 1. USB device

| Item | Value |
|---|---|
| Boot ROM product string | "Remote Download Wireless Adapter" [C] |
| After firmware start | "BCMUSB 802.11 Wireless Adapter", but **only if the host resets the port**. Without a reset the device keeps its address and descriptors [C] |
| VID:PID | `0846:9020` in both states [C] |
| Interface | 1, vendor class `ff/02/ff` [C] |
| Endpoints | `0x81` interrupt IN (**unused**), `0x82` bulk IN, `0x03` bulk OUT [C] |

## 2. Firmware download (RDL)

All requests are vendor control transfers to the interface (`bmRequestType 0xC1` = IN, `0x41` = OUT,
`wValue 0`, `wIndex 0`). The numbering matches mainline `brcmfmac` (`usb.c`).

| bRequest | Dir | brcmfmac name | Observed use |
|---|---|---|---|
| 0 | IN, 8 B | `DL_GETSTATE` | `{u32 state, u32 bytes_received}` — state 0 WAITING, 4 RUNNABLE [C] |
| 2 | IN, 8 B | `DL_GO` | start the downloaded image; replies 8 × `00` [C] |
| 3 | IN, 8 B | `DL_START` | begin download; returns state/bytes [C] |
| 4 | OUT | `DL_REBOOT` | sent by the vendor driver on halt [C]; ignored by running firmware [C] |
| 5 | IN, 8+ B | `DL_GETVER` | first u32: `0x4322` in the boot ROM, **`0xA123` once firmware runs** [C] |
| 8 | IN, 8 B | `DL_RESETCFG` | sent once after `DL_GO` by the vendor driver [C] |
| 9 | IN, `wLength 1928` | *(ours: return read)* | **reply channel for synchronous RPC calls**, see §3 [C] |

The image is a TRX file (`"HDR0"` magic, 28-byte header, gzip payload; the boot ROM inflates it
itself). It's sent on bulk EP `0x03` in 1500-byte chunks, polling `DL_GETSTATE` until
`bytes_received == image length` and the state is RUNNABLE. Then `DL_GO`. 131 072 bytes take
about 0.3–0.5 s [C].

## 3. Transport

```
frame on the wire:   u32le tp_len  |  tp_len bytes of RPC message
```

`tp_len` excludes itself. This matches `BCM_RPC_TP_ENCAP_LEN = 4` in the ISC `bcm_rpc_tp.h` [C].

| Direction | Channel | Notes |
|---|---|---|
| host → dongle | bulk OUT `0x03` | Management and synchronous calls go one per transfer. Async calls are **aggregated**: up to 3 frames concatenated in one transfer with no padding between them [C] |
| dongle → host, async | bulk IN `0x82` | Also aggregated, up to 3 frames [C] |
| dongle → host, sync reply | control IN `bRequest 9` | One read per synchronous call; the data stage holds one transport frame [C] |

The ISC header's limits agree: `BCM_RPC_TP_{HOST,DNGL}_AGG_MAX_SFRAME = 3`. A decoder handled
about 14 900 frames from a full capture without a framing error [C].

## 4. RPC messages

### 4.1 Header [C]

```
u32le  hdr = kind << 24 | session << 16 | seq
```

| kind | *(ours)* | Meaning |
|---|---|---|
| 1 | CALL | one-way call (either direction); no reply |
| 2 | SYNC | host→dongle call that expects a reply, **and** that reply (read via request 9) |
| 3 | MGMT | session management (§4.2) |

- **session:** chosen by the host; any value worked in our tests (`0x01`, `0x69`) [C].
- **seq:** the host increments one counter across all its CALL and SYNC messages. Replies carry the
  **dongle's own counter**, not the call's seq. Match replies to calls in FIFO order (no mismatch
  across 3127 calls in a capture) [C].

### 4.2 Session management (kind 3) [C]

Payload is u32 words: `action, version, word3[, word4]`.

| action | *(ours)* | Direction | Observed |
|---|---|---|---|
| 1 | HELLO | H→D, then echoed D→H | Reply: `1, version, status=1, chip=0x4322` |
| 2 | CONNECT | H→D | `word3` = chip id (vendor driver) or 0 (both accepted) |
| 3 | ACCEPT | D→H | `3, version, status, chip`; status 0 on a first connect, **2 when reconnecting to running firmware** |
| 5 | REFUSE | D→H | sent when `version` doesn't match the firmware: status 3 |

**`version` must equal the running firmware's version exactly**, encoded as
`major<<24 | minor<<16 | build<<8 | patch` (byte-packed): 5.100.148.5 → `0x05649405`. 5.60.180.11
(`0x053cb40b`) was refused by 5.100 firmware with action 5, status 3 [C].

Vendor-driver sequence (session `0x69`):

```
H>D  10000000 | 00006903 | 01000000 05946405 xxxxxxxx              HELLO (word3: unexplained value [?])
D>H  14000000 | 00006903 | 01000000 05946405 01000000 22430000     HELLO reply, chip 0x4322
H>D  10000000 | 00006903 | 02000000 05946405 22430000              CONNECT
D>H  14000000 | 00006903 | 03000000 05946405 00000000 22430000     ACCEPT
```

### 4.3 Calls (kinds 1 and 2) [C]

```
u32 rpc_id | arguments…
```

- **Arguments** are little-endian u32 words.
- **Fixed-size byte fields** such as a MAC address are sent raw and zero-padded to a multiple of 4
  (a MAC is 6 bytes + 2 zero bytes).
- **Variable-length buffers** are preceded by an explicit length argument, then the bytes, padded to
  4. Example: id 52 `(offset, len=8, 8 bytes)`; id 40 `(offset, len=16, 16 bytes, selector)` [C].
- **Replies** (kind 2, via request 9) start with the **echoed rpc_id**, followed by the return values [C].

## 5. rpc_id numbering

The ISC `wlc_rpc.h` enum (firmware API **5.75.11**) gives names, in `tools/data/wlrpc_5_75.txt`.
Firmware 5.100 renumbers it; this was established by matching each call's behaviour in captures
against the enum [C]:

- **ids ≤ 61:** identical to the 5.75 index
- **ids ≥ 65:** 5.75 index **+ 1**
- **ids 62–64:** never observed
- **ids ≥ 153:** new or shifted further; mostly unknown

`tools/rpcids.py` implements this. The table lists the ids the vendor driver used. "args" means
argument words after the id; counts are from one ~3-minute capture.

| id | 5.75 name (shortened) | Kind | Count | Args → reply | Notes |
|---|---|---|---|---|---|
| 1 | REG_READ | sync | 276 | (offset, size) → u32 | **hangs the dongle if issued before BMAC_INIT** [C] |
| 2 | REG_WRITE | call | 16 | (offset, value, size) | |
| 3 | MHF_SET | call | 700 | (idx, mask, value, bands) | |
| 5–8 | UP_PREP / UP_FINISH / DOWN_PREP / DOWN_FINISH | sync | 3 each | → int | |
| 10 | BMAC_RESET | call | 3 | — | |
| 14 | RPC_AGG_SET | call | 1 | (1) | enables dongle→host aggregation [I] |
| 18 | BMAC_INIT | call | 1 | (chanspec, mute) | first seen as `(0x2b0b, 0)` = ch 11 |
| 19 / 47 | SET_CWMIN / SET_CWMAX | call | 1 / 1 | (0xf) / (0x3ff) | |
| 22 / 23 / 24 | PHY_HOLD_UPD / PHY_MUTE_UPD / PHY_CLEAR_TSSI | call | 45 / 3 / 19 | | |
| 26 | PHY_ANT_RXDIV_SET | call | 3 | (3) | |
| 32 | PHY_CAL_PERICAL | call | 1 | (4) | at association |
| 39 | COPYFROM_OBJMEM | sync | 7 | (offset, len, selector) → bytes | |
| 40 | COPYTO_OBJMEM | call | 2 | (offset, len, bytes, selector) | **only during association**, 16 B each, likely key install [I] |
| 41 | ENABLE_MAC | call | 210 | — | |
| 42 | MCTRL | call | 978 | (mask, value) | |
| 44 / 51 | READ_SHM / WRITE_SHM | sync / call | 89 / 1343 | (offset) → u16 / (offset, value) | |
| 45 | READ_TSF | sync | 600 | → u64 | |
| 46 | SET_ADDRMATCH | call | 11 | (idx, MAC) | own MAC, BSSID |
| 48 / 49 | SET_RCMTA / SET_SHM | call | 153 / 5 | | |
| 50 | SUSPEND_MAC_AND_WAIT | call | 207 | — | |
| 52 | WRITE_TEMPLATE_RAM | call | 5 | (offset, len, bytes) | |
| 54 | (TX FIFO resume) | call | 19 | (fifo) | name [I] |
| 56 | HW_ETHERADDR | sync | 1 | → MAC | the dongle's MAC [C] |
| 57 | SET_HW_ETHERADDR | call | 6 | (MAC) | |
| 58 | BMAC_CHANSPEC_SET | call | 204 | (chanspec, …, 164 B incl. tx power limits) | scans step ch 1→11, ~125 ms dwell |
| 59 / 60 | TXANT_SET / ANTSEL_TYPE_SET | call | 8 / 1 | (0xc0) / (0) | |
| 61 | BMAC_TXFIFO | call | 695 | §7.1 | every transmitted frame |
| 65 | SET_SHORTSLOT | call | 2 | (1) | first id with the +1 shift |
| 66 | WAIT_FOR_WAKE | call | 79 | — | |
| 67 / 68 | PHY_TXPOWER_GET_CURRENT / HW_CTRL_GET | sync | 201 / 202 | → 644 B / int | |
| 70 / 71 | PHY_BSSINIT / BAND_STF_SS_SET | call | 1 / 37 | | |
| 74 | PHY_BAND_CHANNELS | sync | 1 | (band) → bitmap | `0x7ffe` = ch 1–14 |
| 75 | BMAC_REVINFO_GET | sync | 1 | → 124 B | word 0 vendor `0x14e4`, word 6 chip `0x4322` [C] |
| 76 | BMAC_STATE_GET | sync | 1 | → 8 B | |
| 77 | BMAC_XMTFIFO_SZ_GET | sync | 6 | (fifo 0–5) → size | `[20, 192, 192, 21, 17, 5]` [C] |
| 85 / 86 / 87 | CH14_WIDE_FILTER / PHY_NOISE_AVG / PHYCHAIN_INIT | | 1 / 1034 / 1 | | |
| 91 | BMAC_COPYFROM_VARS | sync | 1 | → u32 len + NVRAM text | `manfid=0x846 prodid=0x0 rdlid=0x9020 vendid=0x14e4 devid=0x4340 …` [C] |
| 93 / 94 / 96 | BTC_MODE_SET / GET / WIRE_GET | sync | | | |
| 101 | PHY_TXPOWER_GET_TARGET_MAX | sync | 201 | | |
| 104 / 109 / 110 | LED_HW_MASK_INIT / BF_PREEMPT_ENABLE / BMAC_DOIOVARS | | | | |
| 116 / 118 / 119 | LED_BLINK_EVENT / LED_BLINK / LED | call | 50 / 94 / 1500 | | activity LED |
| 133 / 134 | BMAC_HW_UP / SET_TXPWR_PERCENT | call | 1 / 1 | (—) / (100) | |
| 135 / 136 | PHYCHAIN_ACTIVE_GET / BLINK_SYNC | | | | |
| 140 / 141 / 142 | ACIM_NOISEM_RESET / INTERFER_SET / IFSCTL_EDCRS_SET | call | | | |
| 153 | ? | sync | 470 | → 28 B | periodic statistics poll [?] |
| 154 | (5.75 #153 PHY_LDPC_SET?) | call | 1 | (0) | [I] |
| 157, 161 | ? | | 3, 1 | | [?] |

**Dongle → host async calls** (bulk IN `0x82`, kind 1) [C by behaviour]:

| id | 5.75 name | Count | Meaning |
|---|---|---|---|
| 81 | WLC_RECV | 1354 | received 802.11 frame (§7.2) |
| 82 | DOTXSTATUS | 600 | 32-byte status per transmitted frame |
| 83 | HIGH_DPC | 202 | `(4, 0xa5a5a5a5)`; interrupt-like notification [I] |
| 114 | UPDATE_PHY_MODE | 9 | [I] |
| 115 | RESET_BMAC_DONE | 6 | [I] |

## 6. Initialisation sequence (vendor driver) [C]

The first calls after ACCEPT, in order:

```
sync 75 REVINFO_GET → sync 76 STATE_GET → sync 77 XMTFIFO_SZ_GET ×6 (fifo 0..5) → sync 91 COPYFROM_VARS
call 59 TXANT_SET(0xc0), call 87 PHYCHAIN_INIT(3,3), sync 161 ?, sync 56 HW_ETHERADDR, call 59, call 60(0),
sync 68, call 3 MHF_SET ×2, sync 74 BAND_CHANNELS(2), call 85(0), call 154(0), call 14 AGG_SET(1),
sync 93 BTC_MODE_SET(8), sync 110 DOIOVARS, call 134(100), call 109(0), call 104(7), call 133 HW_UP,
call 3, sync 5 UP_PREP, call 3, call 10 BMAC_RESET, call 59, call 26(3), call 18 BMAC_INIT(0x2b0b, 0),
sync 94, sync 96, sync 44 READ_SHM …, call 51 WRITE_SHM ×n …
```

Our own tools reproduced the first ten calls (through `HW_ETHERADDR`) with identical replies [C].

**Full replay [C, 2026-10-04].** `tools/rpcprobe` replayed the vendor driver's complete post-handshake
sequence through `BMAC_UP_FINISH` (id 6): 279 host messages (228 one-way, 51 sync), sent one per
transfer, arguments verbatim. 45 sync replies were byte-identical to the capture, and 6 differed only
in the volatile classes below. The dongle's async frames during init matched the capture exactly:
one `RESET_BMAC_DONE` (115) right after `BMAC_INIT`, then three `UPDATE_PHY_MODE` (114).

| Reply | Behaviour | Treat as |
|---|---|---|
| 75 `REVINFO_GET` words 23–30 | scattered single-bit differences between runs; words 0–22 stable | uninitialised firmware memory past the populated fields [C] |
| 110 `DOIOVARS`, iovar 3 (get, 4 B) | low bits differ between runs; the value isn't passed back in any later call | volatile [I] |
| 1 `REG_READ` 0x65a | changes on every read, also within one capture | free-running counter [C] |
| 153 | 28-byte reply; several words increase over time | statistics/counters poll [I] |

Register reads after `BMAC_INIT` return the captured values where they aren't counters
(e.g. `0x688 → 0x0f03`, `0x120 → 0xc4020403`) [C].

## 7. Data path

### 7.1 TX: `BMAC_TXFIFO` (id 61, host → dongle) [C layout, I field names]

```
u32 61
u32 fifo        3 = probe requests during scans; 1 = data, auth, assoc (best effort); 0 seen once
u32 commit      usually 1; 0 on some data frames (batched)
u32 ?           0xffff
u32 ?           1 or 2
u32 ?           0 or 1
u32 len         bytes that follow
d11 TX header   112 B   (layout = brcmsmac `struct d11txh`, D11_TXH_LEN)
PLCP            6 B
802.11 frame    no FCS
```

For protected data the host sends the **plaintext** payload with the CCMP header filled in. The
`d11txh` IV field carries the same CCMP header, and **the dongle hardware encrypts** [C].

Example (QoS data, ToDS, protected), with addresses sanitised:

```
d11txh: MacTxControlLow 0x060f, MacTxControlHigh 0x0043, FC 0x4188, TxFrameRA 02:00:00:00:00:02
802.11: 88 41 …  02:00:00:00:00:02  02:00:00:00:00:01  02:00:00:00:00:03  …  <CCMP hdr>  <plaintext LLC/IP>
```

### 7.2 RX: `WLC_RECV` (id 81, dongle → host) [C]

```
u32 81
u32 len
38 B   header: brcmsmac `struct d11rxhdr` (RxFrameSize, pad, PhyRxStatus_0..5, RxStatus1, RxStatus2,
       RxTSFTime, RxChan) + low TSF word + RSSI/power bytes (layout of the last 14 B partly [I])
[2 B]  pad, present when RxStatus1 & 0x4
6 B    PLCP
…      802.11 frame (+ MIC if protected) + FCS
```

- The header region is a constant 38 bytes across every received frame in the capture, and
  `RxFrameSize == len − 38` for all 1354 captured frames [C].
- `RxChan >> 3` = channel (`0x0034` → 6) [C].
- Byte 28 of the header region is the RSSI in dBm (signed) [I: plausible values, tracks distance].
- The 802.11 FCS is present and verifies on every unprotected frame; 3 300+ frames received by our
  own tool were all FCS-valid [C].
- Protected frames arrive **already decrypted**, with the CCMP header and MIC still present, so
  their FCS no longer verifies.

### 7.3 Receive setup [C, 2026-10-04]

After `BMAC_UP_FINISH` the MAC is enabled on the `BMAC_INIT` channel. The vendor driver retunes with
this block:

```
call 50 SUSPEND_MAC_AND_WAIT
[call 3 MHF_SET x2]            # host flags; differ between scan steps and end-of-scan
sync 153                       # statistics poll
call 58 BMAC_CHANSPEC_SET      (chanspec, ..., per-channel tx power limits)
sync 67, call 3, sync 68, sync 101   # tx power bookkeeping
call 51 WRITE_SHM, call 42 MCTRL(0x00100000, 0x00100000), call 51, call 42 MCTRL(0x01400000, 0)
sync 45 READ_TSF
call 41 ENABLE_MAC
```

- `MCTRL(mask, value)` writes the d11 MAC control register. Bit `0x00100000` is
  `MCTL_BCNS_PROMISC` in brcmsmac's `d11.h`: accept beacons from any BSS. **Without it, beacons from
  other networks are filtered**, and only broadcast probe requests come through [C].
- The vendor driver's *last* retune of a scan (back to its home channel) clears that bit again
  (`MCTRL(0x00100000, 0)`), so a receive-only listener must set it after retuning [C].
- During reception the dongle also sends `HIGH_DPC` (83) every few seconds [C].

## 8. Hazards and recovery [C]

- **Register access before `BMAC_INIT`** (e.g. id 1 REG_READ at offset `0x688`) hangs the whole
  dongle: no reply, and even `DL_GETVER` times out afterwards.
- **Recovery** from any hang: replug or USB port reset. Firmware lives in RAM only, so the device
  comes back in the boot ROM.
- **Reconnecting** to running firmware without a reset works (ACCEPT with status 2).

## 9. Open questions

- Word 3 of the vendor driver's HELLO message; arbitrary values work.
- ids 157 and 161; ids 62–64. id 153 behaves like a statistics poll, which doesn't match the name the
  5.75 enum gives index 152 (`SET_UCODE_LOADED`): evidence that the +1 rule may break above id 152.
- Exact semantics of the TXFIFO header words 2–5, and the trailing 14 B of the RX header.
- Key install: confirm that id 40's 16-byte payloads are the TK/GTK by deriving the PTK from a
  capture with a known passphrase (PLAN phase 7).
- Periodic/keepalive calls the firmware may require (ids 45 READ_TSF and 153 are polled).
