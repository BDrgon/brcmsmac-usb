// SPDX-License-Identifier: ISC
/*
 * brcmsmac_usb — LOW MAC proxy: brcmsmac's brcms_b_* interface implemented as RPC calls to the
 * dongle firmware (docs/split-map.md §3). Everything here runs in process context with the
 * wl mutex held; synchronous calls sleep.
 *
 * Rule: only RPCs whose argument layout was observed in captures of the vendor driver are sent
 * (docs/protocol.md §5). LOW functions without such evidence are host-side no-ops and are
 * marked as such.
 */
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/ctype.h>
#include <linux/etherdevice.h>
#include <net/mac80211.h>

#include "../bmu.h"
#include "types.h"
#include "d11.h"
#include "pub.h"
#include "main.h"
#include "dma.h"
#include "phy/phy_hal.h"
#include "mac80211_if.h"
#include "debug.h"
#include "bmac.h"

/* ------------------------------------------------------------------------------------------ */
/* rpc helpers */

#define BMAC_MAX_WORDS	8

static int bmac_vpack(__le32 *a, int nwords, va_list ap)
{
	int i;

	if (WARN_ON(nwords > BMAC_MAX_WORDS))
		return -EINVAL;
	for (i = 0; i < nwords; i++)
		a[i] = cpu_to_le32(va_arg(ap, u32));
	return 0;
}

static void bmac_rpc_err(struct bmac_core *c, u32 id, int err)
{
	if (err == -ENODEV || err == -ESHUTDOWN)
		return;		/* device gone or hung; the transport already said so */
	dev_warn_ratelimited(c->dev, "rpc %u failed: %d\n", id, err);
}

int bmac_call(struct bmac_core *c, u32 id, int nwords, ...)
{
	__le32 a[BMAC_MAX_WORDS];
	va_list ap;
	int ret;

	va_start(ap, nwords);
	ret = bmac_vpack(a, nwords, ap);
	va_end(ap);
	if (!ret)
		ret = bmu_call(c->bmu, id, a, nwords * 4, GFP_KERNEL);
	if (ret)
		bmac_rpc_err(c, id, ret);
	return ret;
}

int bmac_call_buf(struct bmac_core *c, u32 id, const void *args, u32 len)
{
	int ret = bmu_call(c->bmu, id, args, len, GFP_KERNEL);

	if (ret)
		bmac_rpc_err(c, id, ret);
	return ret;
}

int bmac_sync(struct bmac_core *c, u32 id, void *ret, u32 retmax, u32 *retlen, int nwords, ...)
{
	__le32 a[BMAC_MAX_WORDS];
	va_list ap;
	int err;

	va_start(ap, nwords);
	err = bmac_vpack(a, nwords, ap);
	va_end(ap);
	if (!err)
		err = bmu_sync(c->bmu, id, a, nwords * 4, ret, retmax, retlen);
	if (err)
		bmac_rpc_err(c, id, err);
	return err;
}

/* Synchronous call returning one u32 (0 on failure). */
u32 bmac_sync_u32(struct bmac_core *c, u32 id, int nwords, ...)
{
	__le32 a[BMAC_MAX_WORDS], r = 0;
	u32 len = 0;
	va_list ap;
	int err;

	va_start(ap, nwords);
	err = bmac_vpack(a, nwords, ap);
	va_end(ap);
	if (!err)
		err = bmu_sync(c->bmu, id, a, nwords * 4, &r, sizeof(r), &len);
	if (err) {
		bmac_rpc_err(c, id, err);
		return 0;
	}
	return len >= 4 ? le32_to_cpu(r) : 0;
}

u32 bmac_reg_read(struct bmac_core *c, u32 off, u32 size)
{
	return bmac_sync_u32(c, BRPC_REG_READ, 2, off, size);
}

void bmac_reg_write(struct bmac_core *c, u32 off, u32 v, u32 size)
{
	bmac_call(c, BRPC_REG_WRITE, 3, off, v, size);
}

static inline struct bmac_core *hwcore(struct brcms_hardware *wlc_hw)
{
	return wlc_hw->d11core;
}

/* ------------------------------------------------------------------------------------------ */
/* attach-time identity */

static void bmac_parse_revinfo(struct bmac_core *c, const u8 *b, u32 len)
{
	u32 *w = (u32 *)&c->rev;
	u32 i, n = min_t(u32, len / 4, sizeof(c->rev) / 4);

	memset(&c->rev, 0, sizeof(c->rev));
	for (i = 0; i < n; i++)
		w[i] = get_unaligned_le32(b + 4 * i);
	/* only the populated bands are meaningful; the rest is uninitialised firmware memory */
	if (c->rev.nbands < 2)
		memset(&c->rev.band[1], 0, sizeof(c->rev.band[1]));
}

static bool nvram_get(const char *vars, u32 len, const char *key, unsigned long *v)
{
	size_t klen = strlen(key);
	const char *p = vars, *end = vars + len;

	while (p < end && *p) {
		size_t n = strnlen(p, end - p);

		if (n > klen && !strncmp(p, key, klen) && p[klen] == '=') {
			char tmp[24];

			strscpy(tmp, p + klen + 1, min_t(size_t, sizeof(tmp), n - klen));
			return !kstrtoul(tmp, 0, v);
		}
		p += n + 1;
	}
	return false;
}

static const char *nvram_get_str(const char *vars, u32 len, const char *key)
{
	size_t klen = strlen(key);
	const char *p = vars, *end = vars + len;

	while (p < end && *p) {
		size_t n = strnlen(p, end - p);

		if (n > klen && !strncmp(p, key, klen) && p[klen] == '=')
			return p + klen + 1;
		p += n + 1;
	}
	return NULL;
}

/* bcma's sprom parser stores antenna gains as Q5.2; 0xff means "not programmed" (2 dBm) */
static s8 nvram_antgain(const char *vars, u32 len, const char *key)
{
	unsigned long v;

	if (!nvram_get(vars, len, key, &v) || (v & 0xff) == 0xff)
		v = 2;
	return (s8)((v & 0x3f) << 2);
}

/* Fill the ssb_sprom fields the HIGH code reads from the NVRAM text (id 91). */
static void bmac_parse_nvram(struct bmac_core *c, const char *vars, u32 len)
{
	struct ssb_sprom *s = &c->sprom;
	unsigned long v;
	const char *cc;

	memset(s, 0, sizeof(*s));
	if (nvram_get(vars, len, "sromrev", &v))
		s->revision = v;
	if (nvram_get(vars, len, "boardrev", &v))
		s->board_rev = v;
	if (nvram_get(vars, len, "boardflags", &v)) {
		s->boardflags_lo = v & 0xffff;
		s->boardflags_hi = v >> 16;
	}
	if (nvram_get(vars, len, "boardflags2", &v)) {
		s->boardflags2_lo = v & 0xffff;
		s->boardflags2_hi = v >> 16;
	}
	if (nvram_get(vars, len, "aa2g", &v))
		s->ant_available_bg = v;
	if (nvram_get(vars, len, "aa5g", &v))
		s->ant_available_a = v;
	s->antenna_gain.a0 = nvram_antgain(vars, len, "ag0");
	s->antenna_gain.a1 = nvram_antgain(vars, len, "ag1");
	s->antenna_gain.a2 = nvram_antgain(vars, len, "ag2");
	s->antenna_gain.a3 = nvram_antgain(vars, len, "ag3");
	if (nvram_get(vars, len, "txchain", &v))
		s->txchain = v;
	if (nvram_get(vars, len, "rxchain", &v))
		s->rxchain = v;
	if (nvram_get(vars, len, "antswitch", &v))
		s->antswitch = v;
	cc = nvram_get_str(vars, len, "ccode");
	if (cc && isalpha(cc[0]) && isalpha(cc[1])) {
		s->alpha2[0] = cc[0];
		s->alpha2[1] = cc[1];
	}
}

/*
 * Read what the HIGH needs to know about the device. All of these calls are read-only and
 * safe before BMAC_INIT (docs/protocol.md §6).
 */
int bmac_core_init(struct bmac_core *c)
{
	u8 *buf;
	u32 len, i;
	int ret;

	buf = kmalloc(BMU_RETURN_WLEN, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	ret = bmac_sync(c, BRPC_REVINFO_GET, buf, BMU_RETURN_WLEN, &len, 0);
	if (ret)
		goto out;
	bmac_parse_revinfo(c, buf, len);

	for (i = 0; i < BMAC_NFIFO; i++)
		c->xmtfifo_sz[i] = bmac_sync_u32(c, BRPC_XMTFIFO_SZ_GET, 1, i);

	ret = bmac_sync(c, BRPC_COPYFROM_VARS, buf, BMU_RETURN_WLEN, &len, 0);
	if (ret)
		goto out;
	if (len > 4) {
		u32 n = min_t(u32, get_unaligned_le32(buf), len - 4);

		bmac_parse_nvram(c, buf + 4, n);
	}

	ret = bmac_sync(c, BRPC_HW_ETHERADDR, buf, BMU_RETURN_WLEN, &len, 0);
	if (ret)
		goto out;
	if (len < ETH_ALEN) {
		ret = -EPROTO;
		goto out;
	}
	memcpy(c->macaddr, buf, ETH_ALEN);

	/* channel bitmap per band (reply: u32 words, bit n = channel n) */
	for (i = 0; i < c->rev.nbands && i < 2; i++) {
		u32 band = c->rev.band[i].bandtype;

		ret = bmac_sync(c, BRPC_PHY_BAND_CHANNELS, buf, BMU_RETURN_WLEN, &len, 1, band);
		if (ret)
			goto out;
		memset(c->band_chanvec[i], 0, sizeof(c->band_chanvec[i]));
		memcpy(c->band_chanvec[i], buf, min_t(u32, len, sizeof(c->band_chanvec[i])));
	}

	c->txpwr_hw_ctrl = !!bmac_sync_u32(c, BRPC_TXPOWER_HW_CTRL_GET, 0);
	c->srl = RETRY_SHORT_DEF_FW;
	c->lrl = RETRY_LONG_DEF_FW;

	dev_info(c->dev,
		 "d11 corerev %u, chip 0x%x rev %u pkg %u, board 0x%x rev 0x%x, %u band(s), phy %u rev %u, radio 0x%x rev %u\n",
		 c->rev.corerev, c->rev.chip, c->rev.chiprev, c->rev.chippkg, c->rev.boardtype,
		 c->rev.boardrev, c->rev.nbands, c->rev.band[0].phytype, c->rev.band[0].phyrev,
		 c->rev.band[0].radioid, c->rev.band[0].radiorev);
	dev_info(c->dev, "sromrev %u boardflags 0x%x/0x%x aa2g %u ag0 %d txchain %u rxchain %u ccode %.2s, 2.4 GHz channels 0x%04x\n",
		 c->sprom.revision, c->sprom.boardflags_lo | c->sprom.boardflags_hi << 16,
		 c->sprom.boardflags2_lo | c->sprom.boardflags2_hi << 16,
		 c->sprom.ant_available_bg, c->sprom.antenna_gain.a0, c->sprom.txchain,
		 c->sprom.rxchain, c->sprom.alpha2[0] ? c->sprom.alpha2 : "--",
		 get_unaligned_le16(c->band_chanvec[0]));
out:
	if (ret)
		dev_err(c->dev, "attach: identity calls failed (%d)\n", ret);
	kfree(buf);
	return ret;
}

/* ------------------------------------------------------------------------------------------ */
/* shared memory, object memory, templates */

u16 brcms_b_read_shm(struct brcms_hardware *wlc_hw, uint offset)
{
	return (u16)bmac_sync_u32(hwcore(wlc_hw), BRPC_READ_SHM, 1, offset);
}

void brcms_b_write_shm(struct brcms_hardware *wlc_hw, uint offset, u16 v)
{
	bmac_call(hwcore(wlc_hw), BRPC_WRITE_SHM, 2, offset, v);
}

void brcms_b_copyfrom_objmem(struct brcms_hardware *wlc_hw, uint offset, void *buf,
			     int len, u32 sel)
{
	struct bmac_core *c = hwcore(wlc_hw);
	u8 *r;
	u32 rlen = 0;

	memset(buf, 0, len);
	r = kmalloc(BMU_RETURN_WLEN, GFP_KERNEL);
	if (!r)
		return;
	/* reply: u32 length, then the bytes */
	if (!bmac_sync(c, BRPC_COPYFROM_OBJMEM, r, BMU_RETURN_WLEN, &rlen, 3, offset, len, sel) &&
	    rlen >= 4)
		memcpy(buf, r + 4, min_t(u32, min_t(u32, get_unaligned_le32(r), rlen - 4), len));
	kfree(r);
}

void brcms_b_copyto_objmem(struct brcms_hardware *wlc_hw, uint offset, const void *buf,
			   int len, u32 sel)
{
	u32 n = ALIGN(len, 4);
	u8 *a = kzalloc(12 + n, GFP_KERNEL);

	if (!a)
		return;
	put_unaligned_le32(offset, a);
	put_unaligned_le32(len, a + 4);
	memcpy(a + 8, buf, len);
	put_unaligned_le32(sel, a + 8 + n);
	bmac_call_buf(hwcore(wlc_hw), BRPC_COPYTO_OBJMEM, a, 12 + n);
	kfree(a);
}

void brcms_b_write_template_ram(struct brcms_hardware *wlc_hw, int offset, int len, void *buf)
{
	u32 n = ALIGN(len, 4);
	u8 *a = kzalloc(8 + n, GFP_KERNEL);

	if (!a)
		return;
	put_unaligned_le32(offset, a);
	put_unaligned_le32(len, a + 4);
	memcpy(a + 8, buf, len);
	bmac_call_buf(hwcore(wlc_hw), BRPC_WRITE_TEMPLATE, a, 8 + n);
	kfree(a);
}

/* ------------------------------------------------------------------------------------------ */
/* MAC control */

void brcms_b_mhf(struct brcms_hardware *wlc_hw, u8 idx, u16 mask, u16 val, int bands)
{
	uint b;

	if (idx >= MHFMAX)
		return;
	/* keep the host shadow that brcmsmac maintained, for debugging and AMPDU code */
	for (b = 0; b < wlc_hw->_nbands; b++) {
		struct brcms_hw_band *band = wlc_hw->bandstate[b];

		if ((bands == BRCM_BAND_ALL) || (band->bandtype == bands) ||
		    (bands == BRCM_BAND_AUTO && band == wlc_hw->band))
			band->mhfs[idx] = (band->mhfs[idx] & ~mask) | val;
	}
	bmac_call(hwcore(wlc_hw), BRPC_MHF_SET, 4, idx, mask, val, (u32)bands);
}

void brcms_b_mctrl(struct brcms_hardware *wlc_hw, u32 mask, u32 val)
{
	if (val & ~mask)
		return;		/* error condition, as upstream */
	wlc_hw->maccontrol = (wlc_hw->maccontrol & ~mask) | val;
	/* the firmware keeps the real register value (with its overrides); always forward */
	bmac_call(hwcore(wlc_hw), BRPC_MCTRL, 2, mask, val);
}

/* The firmware tracks suspend nesting itself (docs/split-map.md §3). */
void brcms_c_suspend_mac_and_wait(struct brcms_c_info *wlc)
{
	wlc->hw->mac_suspend_depth++;
	bmac_call(hwcore(wlc->hw), BRPC_SUSPEND_MAC, 0);
}

void brcms_c_enable_mac(struct brcms_c_info *wlc)
{
	if (wlc->hw->mac_suspend_depth)
		wlc->hw->mac_suspend_depth--;
	bmac_call(hwcore(wlc->hw), BRPC_ENABLE_MAC, 0);
}

void brcms_b_wait_for_wake(struct brcms_hardware *wlc_hw)
{
	bmac_call(hwcore(wlc_hw), BRPC_WAIT_FOR_WAKE, 0);
}

void brcms_b_set_addrmatch(struct brcms_hardware *wlc_hw, int match_reg_offset, const u8 *addr)
{
	u8 a[12] = { };

	put_unaligned_le32(match_reg_offset, a);
	memcpy(a + 4, addr, ETH_ALEN);
	if (match_reg_offset == RCM_MAC_OFFSET)
		memcpy(wlc_hw->etheraddr, addr, ETH_ALEN);
	bmac_call_buf(hwcore(wlc_hw), BRPC_SET_ADDRMATCH, a, sizeof(a));
}

void brcms_b_set_shortslot(struct brcms_hardware *wlc_hw, bool shortslot)
{
	wlc_hw->shortslot = shortslot;
	bmac_call(hwcore(wlc_hw), BRPC_SET_SHORTSLOT, 1, (u32)shortslot);
}

/*
 * RETRYLIMIT_UPD (92) has not been seen on the wire; the firmware starts with the 802.11
 * defaults, which are also mac80211's. Only forward real changes.
 */
void brcms_b_retrylimit_upd(struct brcms_hardware *wlc_hw, u16 SRL, u16 LRL)
{
	struct bmac_core *c = hwcore(wlc_hw);

	wlc_hw->SRL = SRL;
	wlc_hw->LRL = LRL;
	if (SRL == c->srl && LRL == c->lrl)
		return;
	dev_info(c->dev, "retry limits %u/%u (unverified rpc %u)\n", SRL, LRL,
		 BRPC_RETRYLIMIT_UPD);
	if (!bmac_call(c, BRPC_RETRYLIMIT_UPD, 2, SRL, LRL)) {
		c->srl = SRL;
		c->lrl = LRL;
	}
}

static int bmac_read_tsf(struct bmac_core *c, u32 *tsf_l, u32 *tsf_h)
{
	__le32 r[2] = { };
	u32 len = 0;
	int ret;

	ret = bmac_sync(c, BRPC_READ_TSF, r, sizeof(r), &len, 0);
	if (!ret && len < sizeof(r))
		ret = -EPROTO;
	*tsf_l = le32_to_cpu(r[0]);
	*tsf_h = le32_to_cpu(r[1]);
	if (!ret) {
		c->tsf_l = *tsf_l;
		c->tsf_h = *tsf_h;
		c->tsf_read_at = jiffies;
		c->tsf_valid = true;
	}
	return ret;
}

void brcms_b_read_tsf(struct brcms_hardware *wlc_hw, u32 *tsf_l_ptr, u32 *tsf_h_ptr)
{
	bmac_read_tsf(hwcore(wlc_hw), tsf_l_ptr, tsf_h_ptr);
}

/* the TSF may have been reset or rewritten (BMAC_INIT, tsf_set): re-read before trusting it */
void bmac_tsf_invalidate(struct bmac_core *c)
{
	c->tsf_valid = false;
}

/*
 * brcmsmac reads the TSF registers for every received frame to extend the 16-bit RxTSFTime.
 * Over RPC that is one synchronous READ_TSF per frame. The LOW already puts the TSF low word,
 * taken when the frame was received, at byte 24 of the receive header (staging
 * wlc_d11rxhdr.tsf_l; checked against RxTSFTime on hardware), so only the high word is needed:
 * it comes from a cached READ_TSF, refreshed every 10 s. tsf_l wraps every ~71 min, so a low
 * word below the cached one means one wrap since that read.
 */
#define BMAC_TSF_REFRESH	(10 * HZ)

u64 bmac_rx_tsf64(struct bmac_core *c, const struct d11rxhdr *rxh)
{
	u32 rx_l = get_unaligned_le32((const u8 *)rxh + 24);
	u16 rx_0_15 = rxh->RxTSFTime;
	u32 l, h;

	if (!c->tsf_valid || time_after(jiffies, c->tsf_read_at + BMAC_TSF_REFRESH))
		bmac_read_tsf(c, &l, &h);
	h = c->tsf_h;
	if (rx_l < c->tsf_l && c->tsf_l - rx_l > 0x80000000u)
		h++;			/* tsf_l wrapped after our read */
	else if (rx_l > c->tsf_l && rx_l - c->tsf_l > 0x80000000u)
		h--;			/* frame received before a read that followed a wrap */

	/* RxTSFTime is the start of the frame; tsf_l may be a little later */
	l = (rx_l & 0xffff0000) | rx_0_15;
	if ((u16)rx_l < rx_0_15) {
		if (l < 0x10000)
			h--;
		l -= 0x10000;
	}
	return (u64)h << 32 | l;
}

/* ------------------------------------------------------------------------------------------ */
/* channel, antennas, STF */

/*
 * CHANSPEC_SET (58): (chanspec, mute, 156 B of tx power limits). The limits are three 52-byte
 * blocks (20 MHz, 40 MHz, 20-in-40), each cck[4] ofdm[8] ofdm_cdd[8] mcs_siso[8] mcs_cdd[8]
 * mcs_stbc[8] mcs_mimo[8] in quarter dBm (docs/split-map.md §1).
 */
#define BMAC_TXPWR_BLK	52

static u8 *bmac_pack_txpwr(u8 *p, const u8 *cck, const u8 *ofdm, const u8 *ofdm_cdd,
			   const u8 *siso, const u8 *cdd, const u8 *stbc, const u8 *mimo)
{
	if (cck)
		memcpy(p, cck, BRCMS_NUM_RATES_CCK);
	p += BRCMS_NUM_RATES_CCK;
	memcpy(p, ofdm, 8);
	memcpy(p + 8, ofdm_cdd, 8);
	memcpy(p + 16, siso, 8);
	memcpy(p + 24, cdd, 8);
	memcpy(p + 32, stbc, 8);
	memcpy(p + 40, mimo, 8);
	return p + 48;
}

void brcms_b_set_chanspec(struct brcms_hardware *wlc_hw, u16 chanspec, bool mute,
			  struct txpwr_limits *txpwr)
{
	struct bmac_core *c = hwcore(wlc_hw);
	u8 a[8 + 3 * BMAC_TXPWR_BLK] = { };
	u8 *p = a + 8;

	BUILD_BUG_ON(BRCMS_NUM_RATES_CCK + 6 * 8 != BMAC_TXPWR_BLK);

	put_unaligned_le32(chanspec, a);
	put_unaligned_le32(mute, a + 4);
	if (txpwr) {
		p = bmac_pack_txpwr(p, txpwr->cck, txpwr->ofdm, txpwr->ofdm_cdd,
				    txpwr->mcs_20_siso, txpwr->mcs_20_cdd, txpwr->mcs_20_stbc,
				    txpwr->mcs_20_mimo);
		bmac_pack_txpwr(p, NULL, txpwr->ofdm_40_siso, txpwr->ofdm_40_cdd,
				txpwr->mcs_40_siso, txpwr->mcs_40_cdd, txpwr->mcs_40_stbc,
				txpwr->mcs_40_mimo);
		/* third block (20-in-40) left at zero, as the vendor driver sends for HT20 */
	}
	wlc_hw->chanspec = chanspec;
	c->chanspec = chanspec;
	bmac_call_buf(c, BRPC_CHANSPEC_SET, a, sizeof(a));
}

void brcms_b_txant_set(struct brcms_hardware *wlc_hw, u16 phytxant)
{
	wlc_hw->bmac_phytxant = phytxant;
	bmac_call(hwcore(wlc_hw), BRPC_TXANT_SET, 1, phytxant);
}

void brcms_b_antsel_type_set(struct brcms_hardware *wlc_hw, u8 antsel_type)
{
	wlc_hw->antsel_type = antsel_type;
	bmac_call(hwcore(wlc_hw), BRPC_ANTSEL_TYPE_SET, 1, antsel_type);
}

void brcms_b_antsel_set(struct brcms_hardware *wlc_hw, u32 antsel_avail)
{
	wlc_hw->antsel_avail = antsel_avail;
	bmac_call(hwcore(wlc_hw), BRPC_ANTSEL_SET, 1, antsel_avail);
}

void brcms_b_band_stf_ss_set(struct brcms_hardware *wlc_hw, u8 stf_mode)
{
	wlc_hw->hw_stf_ss_opmode = stf_mode;
	bmac_call(hwcore(wlc_hw), BRPC_BAND_STF_SS_SET, 1, stf_mode);
}

/*
 * The vendor driver never mutes through BMAC_MUTE (20); its argument layout is unverified.
 * Transmission is held back on the host instead (bmac_allow_tx, bmac_dma.c).
 */
void brcms_b_mute(struct brcms_hardware *wlc_hw, bool mute_tx)
{
	dev_dbg(hwcore(wlc_hw)->dev, "mute %d (host-side only)\n", mute_tx);
}

/* ------------------------------------------------------------------------------------------ */
/* up / down */

void brcms_b_hw_up(struct brcms_hardware *wlc_hw)
{
	if (wlc_hw->wlc->pub->hw_up)
		return;
	bmac_call(hwcore(wlc_hw), BRPC_HW_UP, 0);
	wlc_hw->ucode_loaded = false;
	wlc_hw->wlc->pub->hw_up = true;
}

int brcms_b_up_prep(struct brcms_hardware *wlc_hw)
{
	struct bmac_core *c = hwcore(wlc_hw);
	__le32 r = 0;
	u32 len = 0;
	int ret;

	ret = bmac_sync(c, BRPC_UP_PREP, &r, sizeof(r), &len, 0);
	if (ret)
		return ret;
	if (len >= 4 && le32_to_cpu(r)) {
		dev_err(c->dev, "UP_PREP returned %d (radio disabled?)\n", (s32)le32_to_cpu(r));
		return -ENOMEDIUM;
	}
	wlc_hw->clk = true;
	return 0;
}

int brcms_b_up_finish(struct brcms_hardware *wlc_hw)
{
	int ret = bmac_sync(hwcore(wlc_hw), BRPC_UP_FINISH, NULL, 0, NULL, 0);

	wlc_hw->up = true;
	brcms_intrson(wlc_hw->wlc->wl);
	return ret;
}

int brcms_b_bmac_down_prep(struct brcms_hardware *wlc_hw)
{
	if (!wlc_hw->up)
		return 0;
	brcms_intrsoff(wlc_hw->wlc->wl);
	if (!brcms_deviceremoved(wlc_hw->wlc))
		bmac_sync(hwcore(wlc_hw), BRPC_DOWN_PREP, NULL, 0, NULL, 0);
	return 0;
}

void brcms_b_reset(struct brcms_hardware *wlc_hw)
{
	bmac_tsf_invalidate(hwcore(wlc_hw));
	bmac_call(hwcore(wlc_hw), BRPC_BMAC_RESET, 0);
}

/* The LOW watchdog (PHY calibration, noise, etc.) runs inside the firmware. */
void brcms_b_watchdog(struct brcms_c_info *wlc)
{
}

bool brcms_b_radio_read_hwdisabled(struct brcms_hardware *wlc_hw)
{
	return false;	/* the WNA3100 has no radio kill switch */
}

void brcms_b_pllreq(struct brcms_hardware *wlc_hw, bool set, u32 req_bit)
{
}

bool brcms_deviceremoved(struct brcms_c_info *wlc)
{
	return READ_ONCE(wlc->hw->d11core->bmu->state) != BMU_ST_UP;
}

/* ------------------------------------------------------------------------------------------ */
/* "interrupts": the LOW reports events with HIGH_DPC messages; nothing to mask on the host */

void brcms_c_intrson(struct brcms_c_info *wlc)
{
	wlc->macintmask = wlc->defmacintmask;
}

u32 brcms_c_intrsoff(struct brcms_c_info *wlc)
{
	u32 m = wlc->macintmask;

	wlc->macintmask = 0;
	return m;
}

void brcms_c_intrsrestore(struct brcms_c_info *wlc, u32 macintmask)
{
	wlc->macintmask = macintmask;
}

bool brcms_c_intrsupd(struct brcms_c_info *wlc)
{
	return true;
}

/* ------------------------------------------------------------------------------------------ */
/* messages from the LOW */

/*
 * WLC_RECV (81): u32 len, then exactly brcmsmac's DMA receive buffer: the 38-byte hardware
 * header (struct d11rxhdr + extras), optional 2-byte pad, PLCP, frame + FCS (docs/protocol.md
 * §7.2). p->data points at the len word. The skb is consumed.
 */
void brcms_c_rpc_recv(struct brcms_c_info *wlc, struct sk_buff *p)
{
	struct d11rxhdr *rxh;
	u32 len;

	if (p->len < 4 + BRCMS_HWRXOFF)
		goto toss;
	len = get_unaligned_le32(p->data);
	skb_pull(p, 4);
	if (len > p->len || len < BRCMS_HWRXOFF)
		goto toss;
	skb_trim(p, len);

	/* the d11rxhdr fields are little-endian u16s; brcmsmac reads them in CPU order */
	rxh = (struct d11rxhdr *)p->data;
	rxh->RxFrameSize = le16_to_cpu((__force __le16)rxh->RxFrameSize);
	rxh->PhyRxStatus_0 = le16_to_cpu((__force __le16)rxh->PhyRxStatus_0);
	rxh->PhyRxStatus_1 = le16_to_cpu((__force __le16)rxh->PhyRxStatus_1);
	rxh->PhyRxStatus_2 = le16_to_cpu((__force __le16)rxh->PhyRxStatus_2);
	rxh->PhyRxStatus_3 = le16_to_cpu((__force __le16)rxh->PhyRxStatus_3);
	rxh->PhyRxStatus_4 = le16_to_cpu((__force __le16)rxh->PhyRxStatus_4);
	rxh->PhyRxStatus_5 = le16_to_cpu((__force __le16)rxh->PhyRxStatus_5);
	rxh->RxStatus1 = le16_to_cpu((__force __le16)rxh->RxStatus1);
	rxh->RxStatus2 = le16_to_cpu((__force __le16)rxh->RxStatus2);
	rxh->RxTSFTime = le16_to_cpu((__force __le16)rxh->RxTSFTime);
	rxh->RxChan = le16_to_cpu((__force __le16)rxh->RxChan);

	brcms_c_recv_frame(wlc, p);
	return;
toss:
	dev_dbg_ratelimited(wlc->hw->d11core->dev, "bad WLC_RECV message (%u bytes)\n", p->len);
	brcmu_pkt_buf_free_skb(p);
}

/*
 * DOTXSTATUS (82): 32 bytes, u16 words: [0] ?, [1] frameid, [2] status, [3] sequence,
 * u32 at 8 = TSF low (docs/protocol.md §7.4).
 */
void brcms_c_rpc_txstatus(struct brcms_c_info *wlc, const u8 *args, u32 len)
{
	struct tx_status txs = { };

	if (len < 12)
		return;
	txs.frameid = get_unaligned_le16(args + 2);
	txs.status = get_unaligned_le16(args + 4);
	txs.sequence = get_unaligned_le16(args + 6);
	txs.lasttxtime = get_unaligned_le32(args + 8);
	brcms_c_txstatus_one(wlc, &txs);
}
