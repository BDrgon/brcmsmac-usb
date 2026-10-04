/* SPDX-License-Identifier: ISC */
/*
 * brcmsmac_usb — LOW MAC proxy for the forked brcmsmac HIGH code.
 *
 * In brcmsmac the LOW MAC (brcms_b_*) drives the d11 core directly over bcma. On the USB BMAC
 * dongle the LOW MAC runs in firmware, so these functions become RPC calls (docs/split-map.md).
 */
#ifndef _BRCMS_BMAC_H_
#define _BRCMS_BMAC_H_

#include <linux/types.h>
#include <linux/skbuff.h>
#include <linux/ssb/ssb.h>

struct bmu_dev;
struct brcms_c_info;
struct brcms_hardware;

/* Stand-in for the bcma d11 core: the transport plus a device for logging. */
struct bmac_core {
	struct bmu_dev *bmu;
	struct device *dev;
	u32 maccontrol;		/* shadow of MCTRL writes (the firmware owns the register) */
	u16 chanspec;		/* last chanspec sent to the PHY */
	u8 hw_txchain, hw_rxchain;
	u16 band_chans[4];	/* channel bitmap from PHY_BAND_CHANNELS (bit n = channel n) */
};

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
	BRPC_BMAC_MUTE = 20,
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
	BRPC_TX_FIFO_SUSPEND = 53,
	BRPC_TX_FIFO_RESUME = 54,
	BRPC_HW_ETHERADDR = 56,
	BRPC_SET_HW_ETHERADDR = 57,
	BRPC_CHANSPEC_SET = 58,
	BRPC_TXANT_SET = 59,
	BRPC_ANTSEL_TYPE_SET = 60,
	BRPC_TXFIFO = 61,
	BRPC_SET_SHORTSLOT = 65,
	BRPC_TXPOWER_HW_CTRL_GET = 68,
	BRPC_BAND_STF_SS_SET = 71,
	BRPC_PHY_BAND_CHANNELS = 74,
	BRPC_REVINFO_GET = 75,
	BRPC_STATE_GET = 76,
	BRPC_XMTFIFO_SZ_GET = 77,
	BRPC_WLC_RECV = 81,
	BRPC_DOTXSTATUS = 82,
	BRPC_HIGH_DPC = 83,
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

/* rpc helpers (process context; the wl mutex is held by callers) */
int bmac_call(struct bmac_core *c, u32 id, int nwords, ...);
int bmac_call_buf(struct bmac_core *c, u32 id, const void *args, u32 len);
int bmac_sync(struct bmac_core *c, u32 id, void *ret, u32 retmax, u32 *retlen, int nwords, ...);
u32 bmac_sync_u32(struct bmac_core *c, u32 id, int nwords, ...);

/* d11 register access for the few HIGH paths that need it (only after BMAC_INIT) */
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

/* attach-time identity (docs/split-map.md §1) */
struct bmac_revinfo {
	u32 vendorid, deviceid, corerev, boardrev, sromrev, chiprev, chip, chippkg;
	u32 boardtype, boardvendor, bustype, buscoretype, buscorerev, issim, nbands;
	struct {
		u32 bandunit, bandtype, radiorev, phytype, phyrev, anarev, radioid, abgphy_encore;
	} band[2];
};
int bmac_get_revinfo(struct bmac_core *c, struct bmac_revinfo *ri);
int bmac_get_sprom(struct bmac_core *c, struct ssb_sprom *sprom, char *ccode);
int bmac_get_macaddr(struct bmac_core *c, u8 *mac);
int bmac_get_xmtfifo_sizes(struct bmac_core *c, u16 *sz, int n);

/* async messages from the LOW (dpc_work, process context, wl mutex held) */
void brcms_c_rpc_recv(struct brcms_c_info *wlc, struct sk_buff *p);
void brcms_c_rpc_txstatus(struct brcms_c_info *wlc, const u8 *args, u32 len);
void brcms_c_rpc_high_dpc(struct brcms_c_info *wlc, u32 macintstatus);

/* emulated TX DMA rings (bmac.c) */
struct dma_pub *bmac_txq_attach(struct brcms_c_info *wlc, uint fifo, uint ntxd);
void bmac_txq_detach(struct dma_pub *pub);

/* glue to the USB transport (mac80211_if.c) */
int brcms_usb_attach(struct bmu_dev *bmu, struct device *dev);
void brcms_usb_detach(struct bmu_dev *bmu);
void brcms_usb_async(void *ctx, u32 id, const u8 *args, u32 len);

#endif /* _BRCMS_BMAC_H_ */
