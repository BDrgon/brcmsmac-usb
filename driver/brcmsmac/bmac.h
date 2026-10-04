/* SPDX-License-Identifier: ISC */
/*
 * brcmsmac_usb — LOW MAC proxy for the forked brcmsmac HIGH code.
 *
 * In brcmsmac the LOW MAC (brcms_b_*) drives the d11 core directly over bcma. On the USB BMAC
 * dongle the LOW MAC runs in firmware, so these functions become RPC calls (docs/split-map.md).
 *
 * Only calls whose argument layout has been seen in captures of the vendor driver are sent
 * (docs/protocol.md §5). LOW functions with no observed RPC are host-side no-ops and say so.
 */
#ifndef _BRCMS_BMAC_H_
#define _BRCMS_BMAC_H_

#include <linux/types.h>
#include <linux/skbuff.h>
#include <linux/device.h>
#include <linux/ssb/ssb.h>

struct bmu_dev;
struct brcms_c_info;
struct brcms_hardware;
struct brcms_info;
struct dma_pub;

/* firmware 5.100.148.5 rpc ids (docs/protocol.md §5) */
enum bmac_rpc_id {
	BRPC_REG_READ = 1,
	BRPC_REG_WRITE = 2,
	BRPC_MHF_SET = 3,
	BRPC_UP_PREP = 5,
	BRPC_UP_FINISH = 6,
	BRPC_DOWN_PREP = 7,
	BRPC_DOWN_FINISH = 8,
	BRPC_BMAC_RESET = 10,
	BRPC_RPC_AGG_SET = 14,
	BRPC_BMAC_INIT = 18,
	BRPC_SET_CWMIN = 19,
	BRPC_PHY_HOLD_UPD = 22,
	BRPC_PHY_MUTE_UPD = 23,
	BRPC_COPYFROM_OBJMEM = 39,
	BRPC_COPYTO_OBJMEM = 40,
	BRPC_ENABLE_MAC = 41,
	BRPC_MCTRL = 42,
	BRPC_READ_SHM = 44,
	BRPC_READ_TSF = 45,
	BRPC_SET_ADDRMATCH = 46,
	BRPC_SET_CWMAX = 47,
	BRPC_SUSPEND_MAC = 50,
	BRPC_WRITE_SHM = 51,
	BRPC_WRITE_TEMPLATE = 52,
	BRPC_HW_ETHERADDR = 56,
	BRPC_CHANSPEC_SET = 58,
	BRPC_TXANT_SET = 59,
	BRPC_ANTSEL_TYPE_SET = 60,
	BRPC_TXFIFO = 61,
	BRPC_SET_SHORTSLOT = 65,
	BRPC_WAIT_FOR_WAKE = 66,
	BRPC_TXPOWER_HW_CTRL_GET = 68,
	BRPC_BAND_STF_SS_SET = 71,
	BRPC_PHY_BAND_CHANNELS = 74,
	BRPC_REVINFO_GET = 75,
	BRPC_STATE_GET = 76,
	BRPC_XMTFIFO_SZ_GET = 77,
	BRPC_WLC_RECV = 81,
	BRPC_DOTXSTATUS = 82,
	BRPC_HIGH_DPC = 83,
	BRPC_FATAL_ERROR = 84,
	BRPC_CH14_WIDE_FILTER = 85,
	BRPC_PHYCHAIN_INIT = 87,
	BRPC_PHYCHAIN_SET = 88,
	BRPC_COPYFROM_VARS = 91,
	BRPC_RETRYLIMIT_UPD = 92,
	BRPC_UPDATE_PHY_MODE = 114,
	BRPC_RESET_BMAC_DONE = 115,
	BRPC_HW_UP = 133,
	BRPC_PHYCHAIN_ACTIVE_GET = 135,
	BRPC_ANTSEL_SET = 154,
	BRPC_PHY_LDPC_SET = 157,
};

/* REVINFO_GET (75) reply, u32 words; 5.100 has corerev before boardrev (docs/split-map.md §1) */
struct bmac_revinfo {
	u32 vendorid, deviceid, corerev, boardrev, sromrev, chiprev, chip, chippkg;
	u32 boardtype, boardvendor, bustype, buscoretype, buscorerev, issim, nbands;
	struct {
		u32 bandunit, bandtype, radiorev, phytype, phyrev, anarev, radioid, abgphy_encore;
	} band[2];
};

#define BMAC_NFIFO	6

/* 802.11 default retry limits; the firmware starts with these */
#define RETRY_SHORT_DEF_FW	7
#define RETRY_LONG_DEF_FW	4

/* size of the hardware receive header in front of every received frame */
#define BRCMS_HWRXOFF		38

/* Stand-in for the bcma d11 core: the RPC transport plus what attach learned about the device. */
struct bmac_core {
	struct bmu_dev *bmu;
	struct device *dev;		/* USB interface, for logging */
	struct brcms_c_info *wlc;
	struct bmac_revinfo rev;
	struct ssb_sprom sprom;		/* filled from the dongle's NVRAM text (id 91) */
	u8 macaddr[6];
	u16 xmtfifo_sz[BMAC_NFIFO];
	u8 band_chanvec[2][32];		/* PHY_BAND_CHANNELS bitmaps (bit n = channel n) */
	u16 chanspec;			/* last chanspec sent to the LOW */
	bool txpwr_hw_ctrl;
	u16 srl, lrl;			/* retry limits the LOW is using */

	/* TSF cache for receive timestamps (bmac_rx_tsf64) */
	bool tsf_valid;
	u32 tsf_h, tsf_l;		/* last READ_TSF reply */
	unsigned long tsf_read_at;	/* jiffies */

	/* last buffer handed to dma_txfast(), sent or not (debugfs last_tx) */
	u8 last_tx[256];
	u32 last_tx_len;		/* full length; at most sizeof(last_tx) kept */
	u8 last_tx_fifo;
	bool last_tx_sent;

	/* TX submit/status log (debugfs txlog) */
	struct bmac_txlog_ent {
		ktime_t t;
		u8 kind;		/* 'S' submitted, 'T' tx status, 'D' dropped (allow_tx=0) */
		u8 fifo;
		u16 frameid, status, fc, mcl, seq;
		u16 len;
	} txlog[128];
	unsigned int txlog_head;

	/* receive/transmit counters (debugfs rxtx) */
	struct {
		u32 frames, prot;
		u32 s1[16], prot_s1[16];	/* RxStatus1 bits, all / protected frames */
		u32 txs, txs_ack, txs_noack, txs_supr;
		u32 fifo_ack[BMAC_NFIFO], fifo_noack[BMAC_NFIFO], fifo_supr[BMAC_NFIFO];
		u32 supr_reason[8];
	} cnt;
};

/* rpc helpers (process context; the wl mutex is held by callers) */
int bmac_call(struct bmac_core *c, u32 id, int nwords, ...);
int bmac_call_buf(struct bmac_core *c, u32 id, const void *args, u32 len);
int bmac_sync(struct bmac_core *c, u32 id, void *ret, u32 retmax, u32 *retlen, int nwords, ...);
u32 bmac_sync_u32(struct bmac_core *c, u32 id, int nwords, ...);

/* d11 register access for the few HIGH paths that need it (refused before BMAC_INIT) */
u32 bmac_reg_read(struct bmac_core *c, u32 off, u32 size);
void bmac_reg_write(struct bmac_core *c, u32 off, u32 v, u32 size);
#define bmac_read32(c, off)		bmac_reg_read(c, off, 4)
#define bmac_read16(c, off)		((u16)bmac_reg_read(c, off, 2))
#define bmac_write32(c, off, v)		bmac_reg_write(c, off, v, 4)
#define bmac_write16(c, off, v)		bmac_reg_write(c, off, v, 2)
#define bmac_set32(c, off, v)		bmac_write32(c, off, bmac_read32(c, off) | (v))
#define bmac_mask32(c, off, m)		bmac_write32(c, off, bmac_read32(c, off) & (m))
#define bmac_set16(c, off, v)		bmac_write16(c, off, bmac_read16(c, off) | (v))
#define bmac_mask16(c, off, m)		bmac_write16(c, off, bmac_read16(c, off) & (m))

/* attach-time identity, read once from the dongle (bmac.c) */
int bmac_core_init(struct bmac_core *c);

/* LOW MAC proxy entry points used by main.c (bmac.c; attach/init glue lives in main.c) */
void brcms_b_hw_up(struct brcms_hardware *wlc_hw);
int brcms_b_up_prep(struct brcms_hardware *wlc_hw);
int brcms_b_up_finish(struct brcms_hardware *wlc_hw);
int brcms_b_bmac_down_prep(struct brcms_hardware *wlc_hw);
int brcms_b_down_finish(struct brcms_hardware *wlc_hw);
void brcms_b_reset(struct brcms_hardware *wlc_hw);
void brcms_b_watchdog(struct brcms_c_info *wlc);
void brcms_b_mute(struct brcms_hardware *wlc_hw, bool mute_tx);
void brcms_b_set_shortslot(struct brcms_hardware *wlc_hw, bool shortslot);
void brcms_b_retrylimit_upd(struct brcms_hardware *wlc_hw, u16 SRL, u16 LRL);
void brcms_b_antsel_set(struct brcms_hardware *wlc_hw, u32 antsel_avail);
void brcms_b_set_addrmatch(struct brcms_hardware *wlc_hw, int match_reg_offset,
			   const u8 *addr);
void brcms_b_read_tsf(struct brcms_hardware *wlc_hw, u32 *tsf_l_ptr, u32 *tsf_h_ptr);
void brcms_b_wait_for_wake(struct brcms_hardware *wlc_hw);
bool brcms_b_radio_read_hwdisabled(struct brcms_hardware *wlc_hw);
void brcms_b_pllreq(struct brcms_hardware *wlc_hw, bool set, u32 req_bit);
bool brcms_deviceremoved(struct brcms_c_info *wlc);

/* 64-bit TSF of a received frame, from its receive header (no RPC per frame) */
struct d11rxhdr;
u64 bmac_rx_tsf64(struct bmac_core *c, const struct d11rxhdr *rxh);
void bmac_tsf_invalidate(struct bmac_core *c);

/* async messages from the LOW (dpc work, process context, wl mutex held) */
void brcms_c_rpc_recv(struct brcms_c_info *wlc, struct sk_buff *p);
void brcms_c_rpc_txstatus(struct brcms_c_info *wlc, const u8 *args, u32 len);
/* ...handed to the HIGH code in main.c */
struct tx_status;
void brcms_c_recv_frame(struct brcms_c_info *wlc, struct sk_buff *p);
void brcms_c_txstatus_one(struct brcms_c_info *wlc, struct tx_status *txs);

/* PHY stand-ins (bmac_phy.c) */
struct brcms_phy_pub;
struct brcms_phy_pub *bmac_phy_attach(struct bmac_core *c, uint bandunit);
void bmac_phy_detach(struct brcms_phy_pub *pi);

void bmac_txlog(struct bmac_core *c, u8 kind, uint fifo, u16 frameid, u16 status, u16 fc,
		u16 mcl, u16 seq, u16 len);

/* emulated TX DMA rings (bmac_dma.c) */
struct dma_pub *bmac_txq_attach(struct brcms_c_info *wlc, uint fifo, uint ntxd);
void bmac_txq_complete_dropped(struct brcms_c_info *wlc);
extern bool bmac_allow_tx;

/* glue to the USB transport (mac80211_if.c) */
int brcms_usb_attach(struct bmu_dev *bmu, struct device *dev);
void brcms_usb_detach(struct bmu_dev *bmu);

#endif /* _BRCMS_BMAC_H_ */
