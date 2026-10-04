// SPDX-License-Identifier: ISC
/*
 * brcmsmac_usb — PHY stand-ins. The PHY and radio are driven by the dongle firmware; the HIGH
 * code still calls a handful of wlc_phy_* functions (docs/split-map.md §4). They become RPCs,
 * host shadows, or no-ops here. Nothing from brcmsmac's phy/ directory is built.
 */
#include <linux/slab.h>
#include <net/mac80211.h>

#include "types.h"
#include "d11.h"
#include "pub.h"
#include "main.h"
#include "phy/phy_hal.h"
#include "bmac.h"

/* The host-side PHY object: just enough state for the HIGH code. */
struct brcms_phy_pub {
	struct bmac_core *core;
	uint bandunit;
	uint txpwr_qdbm;
	bool txpwr_override;
};

struct brcms_phy_pub *bmac_phy_attach(struct bmac_core *c, uint bandunit)
{
	struct brcms_phy_pub *pi = kzalloc(sizeof(*pi), GFP_KERNEL);

	if (!pi)
		return NULL;
	pi->core = c;
	pi->bandunit = bandunit;
	pi->txpwr_qdbm = 19 * BRCMS_TXPWR_DB_FACTOR;
	return pi;
}

void bmac_phy_detach(struct brcms_phy_pub *pi)
{
	kfree(pi);
}

/* same table as brcmsmac phy_cmn.c: PLCP rate nibble → 500 kbit/s units */
static const u8 ofdm_rate_lookup[] = {
	BRCM_RATE_48M,
	BRCM_RATE_24M,
	BRCM_RATE_12M,
	BRCM_RATE_6M,
	BRCM_RATE_54M,
	BRCM_RATE_36M,
	BRCM_RATE_18M,
	BRCM_RATE_9M
};

const u8 *wlc_phy_get_ofdm_rate_lookup(void)
{
	return ofdm_rate_lookup;
}

/* host shadow of the last chanspec sent with BMAC_INIT / CHANSPEC_SET */
u16 wlc_phy_chanspec_get(struct brcms_phy_pub *pi)
{
	return pi->core->chanspec;
}

void wlc_phy_stf_chain_init(struct brcms_phy_pub *pi, u8 txchain, u8 rxchain)
{
	bmac_call(pi->core, BRPC_PHYCHAIN_INIT, 2, txchain, rxchain);
}

/* PHYCHAIN_SET (88) is not in our captures; brcmsmac only calls it when a chain changes. */
void wlc_phy_stf_chain_set(struct brcms_phy_pub *pi, u8 txchain, u8 rxchain)
{
	dev_info(pi->core->dev, "tx/rx chains %u/%u (unverified rpc %u)\n", txchain, rxchain,
		 BRPC_PHYCHAIN_SET);
	bmac_call(pi->core, BRPC_PHYCHAIN_SET, 2, txchain, rxchain);
}

u8 wlc_phy_stf_chain_active_get(struct brcms_phy_pub *pi)
{
	u32 v = bmac_sync_u32(pi->core, BRPC_PHYCHAIN_ACTIVE_GET, 0);

	/* fall back to "all chains active" if the call fails */
	return v ? (u8)v : (pi->core->sprom.rxchain << 4 | pi->core->sprom.txchain);
}

bool wlc_phy_txpower_hw_ctrl_get(struct brcms_phy_pub *pi)
{
	return pi->core->txpwr_hw_ctrl;
}

/*
 * PHY_TXPOWER_GET_CURRENT (67) returns a 644-byte struct whose layout is unverified. stf.c only
 * uses it to choose SISO vs CDD for OFDM; all-zero targets select CDD.
 */
void wlc_phy_txpower_get_current(struct brcms_phy_pub *pi, struct tx_power *power, uint channel)
{
	memset(power, 0, sizeof(*power));
}

int wlc_phy_txpower_set(struct brcms_phy_pub *pi, uint qdbm, bool override)
{
	/* regulatory limits reach the firmware with every CHANSPEC_SET */
	pi->txpwr_qdbm = qdbm;
	pi->txpwr_override = override;
	return 0;
}

int wlc_phy_txpower_get(struct brcms_phy_pub *pi, uint *qdbm, bool *override)
{
	*qdbm = pi->txpwr_qdbm;
	if (override)
		*override = pi->txpwr_override;
	return 0;
}

/* PHY_HOLD_UPD (22): (id, val); the vendor sends (PHY_HOLD_FOR_SCAN, 0/1) around scans */
void wlc_phy_hold_upd(struct brcms_phy_pub *pi, u32 id, bool val)
{
	bmac_call(pi->core, BRPC_PHY_HOLD_UPD, 2, id, (u32)val);
}

/* PHY_MUTE_UPD (23): (val, flags); the vendor sends (0, PHY_MUTE_ALL) on the way down */
void wlc_phy_mute_upd(struct brcms_phy_pub *pi, bool val, u32 flags)
{
	bmac_call(pi->core, BRPC_PHY_MUTE_UPD, 2, (u32)val, flags);
}

void wlc_phy_ldpc_override_set(struct brcms_phy_pub *pi, bool val)
{
	bmac_call(pi->core, BRPC_PHY_LDPC_SET, 1, (u32)val);
}

void wlc_phy_chanspec_band_validch(struct brcms_phy_pub *pi, uint band,
				   struct brcms_chanvec *channels)
{
	struct bmac_core *c = pi->core;
	uint unit = band == BRCM_BAND_5G ? 1 : 0;

	BUILD_BUG_ON(sizeof(channels->vec) > sizeof(c->band_chanvec[0]));
	memcpy(channels->vec, c->band_chanvec[unit], sizeof(channels->vec));
}

void wlc_phy_chanspec_ch14_widefilter_set(struct brcms_phy_pub *pi, bool wide_filter)
{
	bmac_call(pi->core, BRPC_CH14_WIDE_FILTER, 1, (u32)wide_filter);
}

/* only matters for 5 GHz-only rate sets */
void wlc_phy_ofdm_rateset_war(struct brcms_phy_pub *pi, bool war)
{
}

/*
 * The LOW computes RSSI: byte 28 of the 38-byte receive header, signed dBm
 * (docs/protocol.md §7.2). rxh points at the start of that header.
 */
int wlc_phy_rssi_compute(struct brcms_phy_pub *pi, struct d11rxhdr *rxh)
{
	/* as brcmsmac: without valid PHY receive status there is no RSSI */
	if (!(rxh->RxStatus2 & RXS_PHYRXST_VALID))
		return BRCMS_RSSI_INVALID;
	return ((s8 *)rxh)[28];
}

void wlc_phy_noise_sample_intr(struct brcms_phy_pub *pi)
{
}
