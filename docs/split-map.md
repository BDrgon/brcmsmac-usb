# brcmsmac HIGH/LOW split map

How the forked brcmsmac (Linux 6.18.53, `driver/brcmsmac/`) is cut so its upper MAC ("HIGH") drives
the dongle's LOW MAC over RPC (`driver/bmu_*.c`). Evidence comes from our USB captures and hardware
tests (docs/protocol.md), and from the ISC-licensed Linux 2.6.37 staging driver. That staging driver
is Broadcom's own split-MAC code: `wlc_mac80211.c` has the `WLC_HIGH_ONLY` paths, `wlc_bmac.c` the LOW
side, and `wlc_rpc.h` the call list.

**Rule:** brcmsmac's `brcms_b_*` functions are the LOW API. They correspond one-to-one to the old
`wlc_bmac_*` functions, and those are exactly the RPC calls in `wlc_rpc.h`. The HIGH code keeps
calling `brcms_b_*`; we replace their bodies with RPC stubs in `brcmsmac/bmac.c`. LOW-internal
helpers (clocks, core reset, ucode download, DMA engines, PHY internals) are deleted, because the
firmware does that work.

## 1. Facts this rests on

| Fact | Evidence |
|---|---|
| Chip/board: corerev **16**, boardrev 0x1300, chip 0x4322 rev 1, 1 band (2.4 GHz), N-PHY rev 4, radio 0x2056 rev 3 | REVINFO (id 75) on hardware. **5.100 swaps corerev/boardrev** compared with the staging `wlc_bmac_revinfo_t`: word 2 = corerev, word 3 = boardrev |
| Board config (boardflags 0xa00, txchain/rxchain 3, aa2g 3, ccode US, …) | NVRAM text (id 91) |
| brcmsmac's shared-memory map (`M_*` in d11.h) matches this firmware's ucode | every Windows `READ/WRITE_SHM` offset that brcmsmac names matches (`M_PRS_MAXTIME` 0x74, `M_EDCF_QINFO` 0x240, `M_AC_TXLMT` 0x300, …) |
| RX payload = brcmsmac's DMA RX buffer: 38-byte hardware header (`BRCMS_HWRXOFF`) + optional pad + PLCP + frame | capture + protocol.md §7.2 |
| The 38-byte header is staging `struct wlc_d11rxhdr`: `d11rxhdr` (24 B) + `tsf_l` (u32) + `rssi` (s8) + legacy/per-antenna power bytes | staging d11.h; RSSI at byte 28 matches the air |
| TX payload = brcmsmac's DMA TX buffer: `d11txh` + PLCP + frame | protocol.md §7.1; our builders match the vendor byte for byte |
| `CHANSPEC_SET` carries 156 B of power limits = 3 × 52 B blocks (BW20, BW40, BW20-in-40), each `cck[4] ofdm[8] ofdm_cdd[8] mcs_siso[8] mcs_cdd[8] mcs_stbc[8] mcs_mimo[8]` | capture: only the first 52 B are non-zero on 2.4 GHz HT20; OFDM/SISO values follow band edges (ch 11 0x42, ch 1 0x4c) while multi-chain MCS stays 0x38. **[I]**, to verify via id 67 |

## 2. Architecture changes

| brcmsmac piece | In the fork |
|---|---|
| bcma bus driver, `struct bcma_device *d11core` | `struct bmac_core` (our transport handle + `struct device *`); logging macros adapted. Binding happens from `bmu_usb.c` once the RPC session is up |
| `wl->lock` (spinlock_bh) perimeter lock | **mutex**: every HIGH path may now sleep on a sync RPC |
| `.tx` mac80211 op (atomic) | queue skb → `tx_work` (process context) → existing `brcms_c_sendpkt_mac80211()` |
| IRQ + `brcms_isr` + DPC tasklet | **async RPC dispatch**: URB completion queues messages → `dpc_work` (process context, under mutex) handles WLC_RECV → `brcms_c_recv()`, DOTXSTATUS → `brcms_c_dotxstatus()`, HIGH_DPC → `macintstatus` bits |
| `dma.c` TX rings | host emulation (as Broadcom's own `wlc_rpctx`): `dma_txfast()` sends `BMAC_TXFIFO` and keeps the skb on a per-FIFO pending list; `dma_getnexttxp()` pops it on tx status; `txavail` provides flow control |
| `dma.c` RX ring | gone: frames arrive as WLC_RECV messages |
| `phy/` (50 k lines) | gone: the PHY runs in firmware. A small set of `wlc_phy_*` functions the HIGH needs become RPCs or host shadows (§4) |
| `aiutils.c`, `pmu.c`, `ucode_loader.c`, `phy_shim.c`, `led.c` (GPL-2.0) | removed from the build |
| `ssb_sprom` from the bcma bus | `struct ssb_sprom` filled from the NVRAM text (id 91) |
| hardware crypto | **not used**, same as brcmsmac: mac80211 software crypto. The firmware never needs keys; the vendor's id 40 key writes are irrelevant to us |
| TX A-MPDU | **off for v1**: needs extra tx-status words from the LOW. RX aggregation works through mac80211 reordering |

## 3. LOW API → RPC (firmware 5.100 ids)

Argument layouts are from captures (protocol.md §5). "sync" = reply via request 9.

| brcmsmac function | id | Args → reply |
|---|---|---|
| `brcms_b_read_shm` | 44 sync | (off) → u16 |
| `brcms_b_write_shm` | 51 | (off, v) |
| `brcms_b_copyfrom_objmem` | 39 sync | (off, len, sel) → u32 len + bytes |
| `brcms_b_copyto_objmem` | 40 | (off, len, bytes…, sel) |
| `brcms_b_mhf` | 3 | (idx, mask, val, bands) |
| `brcms_b_mctrl` | 42 | (mask, val) |
| `brcms_c_suspend_mac_and_wait` | 50 | — (the firmware tracks nesting) |
| `brcms_c_enable_mac` | 41 | — |
| `brcms_b_set_addrmatch` | 46 | (idx, mac[6]+pad) |
| `brcms_b_write_template_ram` | 52 | (off, len, bytes…) |
| `brcms_b_set_cwmin` / `cwmax` | 19 / 47 | (v) |
| `brcms_b_set_shortslot` | 65 | (bool) |
| `brcms_b_set_chanspec` | 58 | (chanspec, mute, 156 B limits) |
| `brcms_b_txant_set` | 59 | (phytxant) |
| `brcms_b_antsel_type_set` | 60 | (type) |
| `brcms_b_antsel_set` | 154 | (avail) |
| `brcms_b_band_stf_ss_set` | 71 | (mode) |
| `brcms_b_retrylimit_upd` | 92 | (SRL, LRL) |
| `brcms_b_hw_up` | 133 | — |
| `brcms_b_up_prep` / `up_finish` / `bmac_down_prep` / `down_finish` | 5 / 6 / 7 / 8 sync | — → int |
| `brcms_b_reset` | 10 | — |
| `brcms_b_init` | 18 | (chanspec, mute) |
| `brcms_b_read_tsf` | 45 sync | — → tsf_l, tsf_h |
| `brcms_b_tx_fifo_suspend` / `resume` | 53 / 54 | (fifo) |
| `brcms_b_xmtfifo_sz_get` | 77 sync (cached at attach) | (fifo) → blocks |
| HIGH register access (`tsf_cfprep`, `ifs_ctl`, `maccommand`, `tsf_random`, …) | 1 sync / 2 | REG_READ (off, size) → u32 / REG_WRITE (off, v, size). **Only after BMAC_INIT** |
| attach: revinfo, state, vars, MAC | 75, 76, 91, 56 sync | see §1 |

## 4. PHY functions the HIGH still calls

| Function | Replacement |
|---|---|
| `wlc_phy_chanspec_get` | host shadow of the last chanspec sent (58 / 18) |
| `wlc_phy_stf_chain_init` | 87 PHYCHAIN_INIT (txchain, rxchain) |
| `wlc_phy_stf_chain_set` | 88 PHYCHAIN_SET (txchain, rxchain) |
| `wlc_phy_stf_chain_active_get` | 135 sync → `0x33` |
| `wlc_phy_txpower_hw_ctrl_get` | 68 sync → int |
| `wlc_phy_txpower_get_current` | 67 sync → `struct tx_power` (verify the layout before use) |
| `wlc_phy_hold_upd` | 22 (id, set) |
| `wlc_phy_mute_upd` | 23 (mute, flags) |
| `wlc_phy_ldpc_override_set` | 157 (val) |
| `wlc_phy_chanspec_band_validch` | channel bitmap from 74 PHY_BAND_CHANNELS (band) at attach |
| `wlc_phy_chanspec_ch14_widefilter_set` | 85 (bool) |
| `wlc_phy_ofdm_rateset_war` | 108 (bool) — not needed for 2.4-only HT; no-op |
| `wlc_phy_txpower_set` / `_get` | host value only for v1 (regulatory limits are enforced through CHANSPEC_SET) |
| `wlc_phy_rssi_compute` | `wlc_d11rxhdr.rssi` (computed by the LOW) |
| `wlc_phy_noise_sample_intr`, `_watchdog`, `_cal_init`, `_init`, `_por_inform`, `_switch_radio`, `_anacore`, … | LOW-internal: deleted with their callers |

## 5. Things the LOW sends us (async)

| id | Handler |
|---|---|
| 81 WLC_RECV | strip len word → skb → endian fixup → `brcms_c_recv()`; RSSI and TSF from `wlc_d11rxhdr` |
| 82 DOTXSTATUS | parse into `struct tx_status` (frameid, status, sequence) → `brcms_c_dotxstatus()` |
| 83 HIGH_DPC | `(macintstatus, …)` → OR into `wlc->macintstatus` → `brcms_c_dpc()` (TBTT, BCNTPL, …) |
| 114 / 115 | logged; 115 RESET_BMAC_DONE follows BMAC_INIT |

## 6. Deleted from main.c (LOW-internal)

`brcms_b_update_slot_timing`, `brcms_c_write_inits`, `brcms_c_write_mhf`, `brcms_c_ucode_bsinit`,
`brcms_b_core_ioctl`, `brcms_b_core_phy_clk`, `brcms_c_setband_inact`, `brcms_b_txstatus` (register
polling), `brcms_c_mhfdef`, `dmareg`, `brcms_b_attach_dmapio`, `brcms_b_detach_dmapio`,
`brcms_b_info_init`, `brcms_b_wait_for_wake`, `brcms_b_clkctl_clk`, `brcms_c_mctrl_reset/_write`,
the `wake_override`/`mute_override` helpers, `brcms_b_bw_set`, `brcms_b_upd_synthpu`,
`brcms_c_ucode_txant_set`, the OFDM pctl1 table code, `brcms_b_bsinit`, the PHY PLL/clock/reset
helpers, `brcms_b_setband`, `isgoodchip`, `validboardtype`, `get_macaddr`, `xtal`,
`radio_read_hwdisabled` (→ "never disabled"), `brcms_b_corereset`, `corerev_fifofixup`,
`switch_macfreq`, `gpio_init`, the ucode write/download functions, `fifoerrors`, the interrupt
mask/status functions (→ no-ops), `validate_chip_access`, `core_phypll_ctl`, `coredisable`,
`read/write_objmem`, `pllreq` (→ no-op), `brcms_b_coreinit`, the `brcms_b_attach` body (→ RPC
attach), `brcms_b_recv`, `chipmatch*`.

## 7. Known gaps / decisions deferred

- **AMPDU TX** (needs the LOW's tx-status `s2`…`s4` words) → after v1.
- **AP / IBSS beaconing** (`brcms_c_update_beacon_hw` touches `maccommand`): works through REG RPCs
  in principle; untested and out of v1 scope (SPEC F-list is station + monitor).
- **Periodic calls the vendor makes** (86 PHY_NOISE_AVG, 153 CCA stats, 45 READ_TSF, LED blinking):
  not required for basic operation; revisit if the firmware misbehaves when idle.
- **TX power reporting** (67) layout unverified; `stf.c` uses it only for the 1-vs-2-chain decision
  at OFDM rates, so v1 uses the NVRAM chain masks.
