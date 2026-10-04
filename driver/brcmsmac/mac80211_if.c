/*
 * Copyright (c) 2010 Broadcom Corporation
 * Copyright (c) 2013 Hauke Mehrtens <hauke@hauke-m.de>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
 * OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#define __UNDEF_NO_VERSION__
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/etherdevice.h>
#include <linux/sched.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/string_choices.h>
#include <net/mac80211.h>
#include <defs.h>
#include "../bmu.h"
#include "phy/phy_hal.h"
#include "d11.h"
#include "channel.h"
#include "scb.h"
#include "pub.h"
#include "mac80211_if.h"
#include "main.h"
#include "debug.h"
#include "bmac.h"

#define N_TX_QUEUES	4 /* #tx queues on mac80211<->driver interface */
#define BRCMS_FLUSH_TIMEOUT	500 /* msec */

/* Flags we support */
#define MAC_FILTERS (FIF_ALLMULTI | \
	FIF_FCSFAIL | \
	FIF_CONTROL | \
	FIF_OTHER_BSS | \
	FIF_BCN_PRBRESP_PROMISC | \
	FIF_PSPOLL)

#define CHAN2GHZ(channel, frequency, chflags)  { \
	.band = NL80211_BAND_2GHZ, \
	.center_freq = (frequency), \
	.hw_value = (channel), \
	.flags = chflags, \
	.max_antenna_gain = 0, \
	.max_power = 19, \
}

#define CHAN5GHZ(channel, chflags)  { \
	.band = NL80211_BAND_5GHZ, \
	.center_freq = 5000 + 5*(channel), \
	.hw_value = (channel), \
	.flags = chflags, \
	.max_antenna_gain = 0, \
	.max_power = 21, \
}

#define RATE(rate100m, _flags) { \
	.bitrate = (rate100m), \
	.flags = (_flags), \
	.hw_value = (rate100m / 5), \
}

static int n_adapters_found;

/* the adapter's NVRAM carries a country code; passing it to cfg80211 changes the system's
 * regulatory domain, so it is opt-in */
static bool reg_hint;
module_param(reg_hint, bool, 0444);
MODULE_PARM_DESC(reg_hint, "pass the adapter's country code to cfg80211 as a regulatory hint");

static bool attach_mac80211 = true;
module_param(attach_mac80211, bool, 0444);
MODULE_PARM_DESC(attach_mac80211, "register with mac80211 (0: transport and debugfs only)");

/*
 * Module parameter for setting the debug message level. Available
 * flags are specified by the BRCM_DL_* macros in
 * drivers/net/wireless/brcm80211/include/defs.h.
 */
module_param_named(debug, brcm_msg_level, uint, 0644);
MODULE_PARM_DESC(debug, "brcmsmac debug message bits (BRCM_DL_*)");

static struct ieee80211_channel brcms_2ghz_chantable[] = {
	CHAN2GHZ(1, 2412, IEEE80211_CHAN_NO_HT40MINUS),
	CHAN2GHZ(2, 2417, IEEE80211_CHAN_NO_HT40MINUS),
	CHAN2GHZ(3, 2422, IEEE80211_CHAN_NO_HT40MINUS),
	CHAN2GHZ(4, 2427, IEEE80211_CHAN_NO_HT40MINUS),
	CHAN2GHZ(5, 2432, 0),
	CHAN2GHZ(6, 2437, 0),
	CHAN2GHZ(7, 2442, 0),
	CHAN2GHZ(8, 2447, IEEE80211_CHAN_NO_HT40PLUS),
	CHAN2GHZ(9, 2452, IEEE80211_CHAN_NO_HT40PLUS),
	CHAN2GHZ(10, 2457, IEEE80211_CHAN_NO_HT40PLUS),
	CHAN2GHZ(11, 2462, IEEE80211_CHAN_NO_HT40PLUS),
	CHAN2GHZ(12, 2467,
		 IEEE80211_CHAN_NO_IR |
		 IEEE80211_CHAN_NO_HT40PLUS),
	CHAN2GHZ(13, 2472,
		 IEEE80211_CHAN_NO_IR |
		 IEEE80211_CHAN_NO_HT40PLUS),
	CHAN2GHZ(14, 2484,
		 IEEE80211_CHAN_NO_IR |
		 IEEE80211_CHAN_NO_HT40PLUS | IEEE80211_CHAN_NO_HT40MINUS |
		 IEEE80211_CHAN_NO_OFDM)
};

static struct ieee80211_channel brcms_5ghz_nphy_chantable[] = {
	/* UNII-1 */
	CHAN5GHZ(36, IEEE80211_CHAN_NO_HT40MINUS),
	CHAN5GHZ(40, IEEE80211_CHAN_NO_HT40PLUS),
	CHAN5GHZ(44, IEEE80211_CHAN_NO_HT40MINUS),
	CHAN5GHZ(48, IEEE80211_CHAN_NO_HT40PLUS),
	/* UNII-2 */
	CHAN5GHZ(52,
		 IEEE80211_CHAN_RADAR |
		 IEEE80211_CHAN_NO_IR | IEEE80211_CHAN_NO_HT40MINUS),
	CHAN5GHZ(56,
		 IEEE80211_CHAN_RADAR |
		 IEEE80211_CHAN_NO_IR | IEEE80211_CHAN_NO_HT40PLUS),
	CHAN5GHZ(60,
		 IEEE80211_CHAN_RADAR |
		 IEEE80211_CHAN_NO_IR | IEEE80211_CHAN_NO_HT40MINUS),
	CHAN5GHZ(64,
		 IEEE80211_CHAN_RADAR |
		 IEEE80211_CHAN_NO_IR | IEEE80211_CHAN_NO_HT40PLUS),
	/* MID */
	CHAN5GHZ(100,
		 IEEE80211_CHAN_RADAR |
		 IEEE80211_CHAN_NO_IR | IEEE80211_CHAN_NO_HT40MINUS),
	CHAN5GHZ(104,
		 IEEE80211_CHAN_RADAR |
		 IEEE80211_CHAN_NO_IR | IEEE80211_CHAN_NO_HT40PLUS),
	CHAN5GHZ(108,
		 IEEE80211_CHAN_RADAR |
		 IEEE80211_CHAN_NO_IR | IEEE80211_CHAN_NO_HT40MINUS),
	CHAN5GHZ(112,
		 IEEE80211_CHAN_RADAR |
		 IEEE80211_CHAN_NO_IR | IEEE80211_CHAN_NO_HT40PLUS),
	CHAN5GHZ(116,
		 IEEE80211_CHAN_RADAR |
		 IEEE80211_CHAN_NO_IR | IEEE80211_CHAN_NO_HT40MINUS),
	CHAN5GHZ(120,
		 IEEE80211_CHAN_RADAR |
		 IEEE80211_CHAN_NO_IR | IEEE80211_CHAN_NO_HT40PLUS),
	CHAN5GHZ(124,
		 IEEE80211_CHAN_RADAR |
		 IEEE80211_CHAN_NO_IR | IEEE80211_CHAN_NO_HT40MINUS),
	CHAN5GHZ(128,
		 IEEE80211_CHAN_RADAR |
		 IEEE80211_CHAN_NO_IR | IEEE80211_CHAN_NO_HT40PLUS),
	CHAN5GHZ(132,
		 IEEE80211_CHAN_RADAR |
		 IEEE80211_CHAN_NO_IR | IEEE80211_CHAN_NO_HT40MINUS),
	CHAN5GHZ(136,
		 IEEE80211_CHAN_RADAR |
		 IEEE80211_CHAN_NO_IR | IEEE80211_CHAN_NO_HT40PLUS),
	CHAN5GHZ(140,
		 IEEE80211_CHAN_RADAR |
		 IEEE80211_CHAN_NO_IR | IEEE80211_CHAN_NO_HT40PLUS |
		 IEEE80211_CHAN_NO_HT40MINUS),
	/* UNII-3 */
	CHAN5GHZ(149, IEEE80211_CHAN_NO_HT40MINUS),
	CHAN5GHZ(153, IEEE80211_CHAN_NO_HT40PLUS),
	CHAN5GHZ(157, IEEE80211_CHAN_NO_HT40MINUS),
	CHAN5GHZ(161, IEEE80211_CHAN_NO_HT40PLUS),
	CHAN5GHZ(165, IEEE80211_CHAN_NO_HT40PLUS | IEEE80211_CHAN_NO_HT40MINUS)
};

/*
 * The rate table is used for both 2.4G and 5G rates. The
 * latter being a subset as it does not support CCK rates.
 */
static struct ieee80211_rate legacy_ratetable[] = {
	RATE(10, 0),
	RATE(20, IEEE80211_RATE_SHORT_PREAMBLE),
	RATE(55, IEEE80211_RATE_SHORT_PREAMBLE),
	RATE(110, IEEE80211_RATE_SHORT_PREAMBLE),
	RATE(60, 0),
	RATE(90, 0),
	RATE(120, 0),
	RATE(180, 0),
	RATE(240, 0),
	RATE(360, 0),
	RATE(480, 0),
	RATE(540, 0),
};

static const struct ieee80211_supported_band brcms_band_2GHz_nphy_template = {
	.band = NL80211_BAND_2GHZ,
	.channels = brcms_2ghz_chantable,
	.n_channels = ARRAY_SIZE(brcms_2ghz_chantable),
	.bitrates = legacy_ratetable,
	.n_bitrates = ARRAY_SIZE(legacy_ratetable),
	.ht_cap = {
		   /* from include/linux/ieee80211.h */
		   .cap = IEEE80211_HT_CAP_GRN_FLD |
			  IEEE80211_HT_CAP_SGI_20 | IEEE80211_HT_CAP_SGI_40,
		   .ht_supported = true,
		   .ampdu_factor = IEEE80211_HT_MAX_AMPDU_64K,
		   .ampdu_density = AMPDU_DEF_MPDU_DENSITY,
		   .mcs = {
			   /* placeholders for now */
			   .rx_mask = {0xff, 0xff, 0, 0, 0, 0, 0, 0, 0, 0},
			   .rx_highest = cpu_to_le16(500),
			   .tx_params = IEEE80211_HT_MCS_TX_DEFINED}
		   }
};

static const struct ieee80211_supported_band brcms_band_5GHz_nphy_template = {
	.band = NL80211_BAND_5GHZ,
	.channels = brcms_5ghz_nphy_chantable,
	.n_channels = ARRAY_SIZE(brcms_5ghz_nphy_chantable),
	.bitrates = legacy_ratetable + BRCMS_LEGACY_5G_RATE_OFFSET,
	.n_bitrates = ARRAY_SIZE(legacy_ratetable) -
			BRCMS_LEGACY_5G_RATE_OFFSET,
	.ht_cap = {
		   .cap = IEEE80211_HT_CAP_GRN_FLD | IEEE80211_HT_CAP_SGI_20 |
			  IEEE80211_HT_CAP_SGI_40,
		   .ht_supported = true,
		   .ampdu_factor = IEEE80211_HT_MAX_AMPDU_64K,
		   .ampdu_density = AMPDU_DEF_MPDU_DENSITY,
		   .mcs = {
			   /* placeholders for now */
			   .rx_mask = {0xff, 0xff, 0, 0, 0, 0, 0, 0, 0, 0},
			   .rx_highest = cpu_to_le16(500),
			   .tx_params = IEEE80211_HT_MCS_TX_DEFINED}
		   }
};

/* flags the given rate in rateset as requested */
static void brcms_set_basic_rate(struct brcm_rateset *rs, u16 rate, bool is_br)
{
	u32 i;

	for (i = 0; i < rs->count; i++) {
		if (rate != (rs->rates[i] & 0x7f))
			continue;

		if (is_br)
			rs->rates[i] |= BRCMS_RATE_FLAG;
		else
			rs->rates[i] &= BRCMS_RATE_MASK;
		return;
	}
}

/*
 * This function frees the WL per-device resources.
 *
 * This function frees resources owned by the WL device pointed to
 * by the wl parameter.
 *
 * precondition: can both be called locked and unlocked
 */
static void brcms_free(struct brcms_info *wl)
{
	struct brcms_timer *t, *next;

	if (wl->pub) {
		brcms_debugfs_detach(wl->pub);
		brcms_c_module_unregister(wl->pub, "linux", wl);
	}

	/* free common resources */
	if (wl->wlc) {
		brcms_c_detach(wl->wlc);
		wl->wlc = NULL;
		wl->pub = NULL;
	}

	/* wait for all pending callbacks to complete */
	while (atomic_read(&wl->callbacks) > 0)
		schedule();

	/* free timers */
	for (t = wl->timers; t; t = next) {
		next = t->next;
#ifdef DEBUG
		kfree(t->name);
#endif
		kfree(t);
	}
}

/*
 * mac80211 may call .tx in atomic context, while sending a frame means RPCs. Queue it for
 * tx_work. rate_driver_data[0] is set here because the d11 header code does not read the
 * tx_info fields it shares storage with.
 */
static void brcms_ops_tx(struct ieee80211_hw *hw,
			 struct ieee80211_tx_control *control,
			 struct sk_buff *skb)
{
	struct brcms_info *wl = hw->priv;
	struct ieee80211_tx_info *tx_info = IEEE80211_SKB_CB(skb);

	tx_info->rate_driver_data[0] = control->sta;
	skb_queue_tail(&wl->txq, skb);
	schedule_work(&wl->tx_work);
}

static void brcms_tx_work(struct work_struct *work)
{
	struct brcms_info *wl = container_of(work, struct brcms_info, tx_work);
	struct ieee80211_hw *hw = wl->pub->ieee_hw;
	struct sk_buff *skb;

	mutex_lock(&wl->lock);
	while ((skb = skb_dequeue(&wl->txq))) {
		if (!wl->pub->up) {
			ieee80211_free_txskb(hw, skb);
			continue;
		}
		brcms_c_sendpkt_mac80211(wl->wlc, skb, hw);
	}
	bmac_txq_complete_dropped(wl->wlc);
	mutex_unlock(&wl->lock);
}

static int brcms_ops_start(struct ieee80211_hw *hw)
{
	struct brcms_info *wl = hw->priv;
	bool blocked;
	int err;

	ieee80211_wake_queues(hw);
	mutex_lock(&wl->lock);
	blocked = brcms_rfkill_set_hw_state(wl);
	mutex_unlock(&wl->lock);
	if (!blocked)
		wiphy_rfkill_stop_polling(wl->pub->ieee_hw->wiphy);

	mutex_lock(&wl->lock);
	/* avoid acknowledging frames before a non-monitor device is added */
	wl->mute_tx = true;

	if (!wl->pub->up)
		if (!blocked)
			err = brcms_up(wl);
		else
			err = -ERFKILL;
	else
		err = -ENODEV;
	mutex_unlock(&wl->lock);

	if (err != 0)
		brcms_err(wl->wlc->hw->d11core, "%s: brcms_up() returned %d\n",
			  __func__, err);
	return err;
}

static void brcms_ops_stop(struct ieee80211_hw *hw, bool suspend)
{
	struct brcms_info *wl = hw->priv;

	ieee80211_stop_queues(hw);

	if (wl->wlc == NULL)
		return;

	/* put driver in down state */
	mutex_lock(&wl->lock);
	brcms_down(wl);
	mutex_unlock(&wl->lock);
}

static int
brcms_ops_add_interface(struct ieee80211_hw *hw, struct ieee80211_vif *vif)
{
	struct brcms_info *wl = hw->priv;

	/* Just STA, AP and ADHOC for now */
	if (vif->type != NL80211_IFTYPE_STATION &&
	    vif->type != NL80211_IFTYPE_AP &&
	    vif->type != NL80211_IFTYPE_ADHOC) {
		brcms_err(wl->wlc->hw->d11core,
			  "%s: Attempt to add type %d, only STA, AP and AdHoc for now\n",
			  __func__, vif->type);
		return -EOPNOTSUPP;
	}

	mutex_lock(&wl->lock);
	wl->wlc->vif = vif;
	wl->mute_tx = false;
	brcms_c_mute(wl->wlc, false);
	if (vif->type == NL80211_IFTYPE_STATION)
		brcms_c_start_station(wl->wlc, vif->addr);
	else if (vif->type == NL80211_IFTYPE_AP)
		brcms_c_start_ap(wl->wlc, vif->addr, vif->bss_conf.bssid,
				 vif->cfg.ssid, vif->cfg.ssid_len);
	else if (vif->type == NL80211_IFTYPE_ADHOC)
		brcms_c_start_adhoc(wl->wlc, vif->addr);
	mutex_unlock(&wl->lock);

	return 0;
}

static void
brcms_ops_remove_interface(struct ieee80211_hw *hw, struct ieee80211_vif *vif)
{
	struct brcms_info *wl = hw->priv;

	mutex_lock(&wl->lock);
	wl->wlc->vif = NULL;
	mutex_unlock(&wl->lock);
}

static int brcms_ops_config(struct ieee80211_hw *hw, int radio_idx,
			    u32 changed)
{
	struct ieee80211_conf *conf = &hw->conf;
	struct brcms_info *wl = hw->priv;
	struct bmac_core *core = wl->wlc->hw->d11core;
	int err = 0;
	int new_int;

	mutex_lock(&wl->lock);
	if (changed & IEEE80211_CONF_CHANGE_LISTEN_INTERVAL) {
		brcms_c_set_beacon_listen_interval(wl->wlc,
						   conf->listen_interval);
	}
	if (changed & IEEE80211_CONF_CHANGE_MONITOR)
		brcms_dbg_info(core, "%s: change monitor mode: %s\n", __func__,
			       str_true_false(conf->flags &
					      IEEE80211_CONF_MONITOR));
	if (changed & IEEE80211_CONF_CHANGE_PS)
		brcms_err(core, "%s: change power-save mode: %s (implement)\n",
			  __func__,
			  str_true_false(conf->flags & IEEE80211_CONF_PS));

	if (changed & IEEE80211_CONF_CHANGE_POWER) {
		err = brcms_c_set_tx_power(wl->wlc, conf->power_level);
		if (err < 0) {
			brcms_err(core, "%s: Error setting power_level\n",
				  __func__);
			goto config_out;
		}
		new_int = brcms_c_get_tx_power(wl->wlc);
		if (new_int != conf->power_level)
			brcms_err(core,
				  "%s: Power level req != actual, %d %d\n",
				  __func__, conf->power_level,
				  new_int);
	}
	if (changed & IEEE80211_CONF_CHANGE_CHANNEL) {
		if (conf->chandef.width == NL80211_CHAN_WIDTH_20 ||
		    conf->chandef.width == NL80211_CHAN_WIDTH_20_NOHT)
			err = brcms_c_set_channel(wl->wlc,
						  conf->chandef.chan->hw_value);
		else
			err = -ENOTSUPP;
	}
	if (changed & IEEE80211_CONF_CHANGE_RETRY_LIMITS)
		err = brcms_c_set_rate_limit(wl->wlc,
					     conf->short_frame_max_tx_count,
					     conf->long_frame_max_tx_count);

 config_out:
	mutex_unlock(&wl->lock);
	return err;
}

static void
brcms_ops_bss_info_changed(struct ieee80211_hw *hw,
			struct ieee80211_vif *vif,
			struct ieee80211_bss_conf *info, u64 changed)
{
	struct brcms_info *wl = hw->priv;
	struct bmac_core *core = wl->wlc->hw->d11core;

	if (changed & BSS_CHANGED_ASSOC) {
		/* association status changed (associated/disassociated)
		 * also implies a change in the AID.
		 */
		brcms_err(core, "%s: %s: %sassociated\n", KBUILD_MODNAME,
			  __func__, vif->cfg.assoc ? "" : "dis");
		mutex_lock(&wl->lock);
		brcms_c_associate_upd(wl->wlc, vif->cfg.assoc);
		mutex_unlock(&wl->lock);
	}
	if (changed & BSS_CHANGED_ERP_SLOT) {
		s8 val;

		/* slot timing changed */
		if (info->use_short_slot)
			val = 1;
		else
			val = 0;
		mutex_lock(&wl->lock);
		brcms_c_set_shortslot_override(wl->wlc, val);
		mutex_unlock(&wl->lock);
	}

	if (changed & BSS_CHANGED_HT) {
		/* 802.11n parameters changed */
		u16 mode = info->ht_operation_mode;

		mutex_lock(&wl->lock);
		brcms_c_protection_upd(wl->wlc, BRCMS_PROT_N_CFG,
			mode & IEEE80211_HT_OP_MODE_PROTECTION);
		brcms_c_protection_upd(wl->wlc, BRCMS_PROT_N_NONGF,
			mode & IEEE80211_HT_OP_MODE_NON_GF_STA_PRSNT);
		brcms_c_protection_upd(wl->wlc, BRCMS_PROT_N_OBSS,
			mode & IEEE80211_HT_OP_MODE_NON_HT_STA_PRSNT);
		mutex_unlock(&wl->lock);
	}
	if (changed & BSS_CHANGED_BASIC_RATES) {
		struct ieee80211_supported_band *bi;
		u32 br_mask, i;
		u16 rate;
		struct brcm_rateset rs;
		int error;

		/* retrieve the current rates */
		mutex_lock(&wl->lock);
		brcms_c_get_current_rateset(wl->wlc, &rs);
		mutex_unlock(&wl->lock);

		br_mask = info->basic_rates;
		bi = hw->wiphy->bands[brcms_c_get_curband(wl->wlc)];
		for (i = 0; i < bi->n_bitrates; i++) {
			/* convert to internal rate value */
			rate = (bi->bitrates[i].bitrate << 1) / 10;

			/* set/clear basic rate flag */
			brcms_set_basic_rate(&rs, rate, br_mask & 1);
			br_mask >>= 1;
		}

		/* update the rate set */
		mutex_lock(&wl->lock);
		error = brcms_c_set_rateset(wl->wlc, &rs);
		mutex_unlock(&wl->lock);
		if (error)
			brcms_err(core, "changing basic rates failed: %d\n",
				  error);
	}
	if (changed & BSS_CHANGED_BEACON_INT) {
		/* Beacon interval changed */
		mutex_lock(&wl->lock);
		brcms_c_set_beacon_period(wl->wlc, info->beacon_int);
		mutex_unlock(&wl->lock);
	}
	if (changed & BSS_CHANGED_BSSID) {
		/* BSSID changed, for whatever reason (IBSS and managed mode) */
		mutex_lock(&wl->lock);
		brcms_c_set_addrmatch(wl->wlc, RCM_BSSID_OFFSET, info->bssid);
		mutex_unlock(&wl->lock);
	}
	if (changed & BSS_CHANGED_SSID) {
		/* BSSID changed, for whatever reason (IBSS and managed mode) */
		mutex_lock(&wl->lock);
		brcms_c_set_ssid(wl->wlc, vif->cfg.ssid, vif->cfg.ssid_len);
		mutex_unlock(&wl->lock);
	}
	if (changed & BSS_CHANGED_BEACON) {
		/* Beacon data changed, retrieve new beacon (beaconing modes) */
		struct sk_buff *beacon;
		u16 tim_offset = 0;

		mutex_lock(&wl->lock);
		beacon = ieee80211_beacon_get_tim(hw, vif, &tim_offset, NULL, 0);
		brcms_c_set_new_beacon(wl->wlc, beacon, tim_offset,
				       info->dtim_period);
		mutex_unlock(&wl->lock);
	}

	if (changed & BSS_CHANGED_AP_PROBE_RESP) {
		struct sk_buff *probe_resp;

		mutex_lock(&wl->lock);
		probe_resp = ieee80211_proberesp_get(hw, vif);
		brcms_c_set_new_probe_resp(wl->wlc, probe_resp);
		mutex_unlock(&wl->lock);
	}

	if (changed & BSS_CHANGED_BEACON_ENABLED) {
		/* Beaconing should be enabled/disabled (beaconing modes) */
		brcms_err(core, "%s: Beacon enabled: %s\n", __func__,
			  str_true_false(info->enable_beacon));
		if (info->enable_beacon &&
		    hw->wiphy->flags & WIPHY_FLAG_AP_PROBE_RESP_OFFLOAD) {
			brcms_c_enable_probe_resp(wl->wlc, true);
		} else {
			brcms_c_enable_probe_resp(wl->wlc, false);
		}
	}

	if (changed & BSS_CHANGED_CQM) {
		/* Connection quality monitor config changed */
		brcms_err(core, "%s: cqm change: threshold %d, hys %d "
			  " (implement)\n", __func__, info->cqm_rssi_thold,
			  info->cqm_rssi_hyst);
	}

	if (changed & BSS_CHANGED_IBSS) {
		/* IBSS join status changed */
		brcms_err(core, "%s: IBSS joined: %s (implement)\n",
			  __func__, str_true_false(vif->cfg.ibss_joined));
	}

	if (changed & BSS_CHANGED_ARP_FILTER) {
		/* Hardware ARP filter address list or state changed */
		brcms_err(core, "%s: arp filtering: %d addresses"
			  " (implement)\n", __func__, vif->cfg.arp_addr_cnt);
	}

	if (changed & BSS_CHANGED_QOS) {
		/*
		 * QoS for this association was enabled/disabled.
		 * Note that it is only ever disabled for station mode.
		 */
		brcms_err(core, "%s: qos enabled: %s (implement)\n",
			  __func__, str_true_false(info->qos));
	}
	return;
}

static void
brcms_ops_configure_filter(struct ieee80211_hw *hw,
			unsigned int changed_flags,
			unsigned int *total_flags, u64 multicast)
{
	struct brcms_info *wl = hw->priv;
	struct bmac_core *core = wl->wlc->hw->d11core;

	changed_flags &= MAC_FILTERS;
	*total_flags &= MAC_FILTERS;

	if (changed_flags & FIF_ALLMULTI)
		brcms_dbg_info(core, "FIF_ALLMULTI\n");
	if (changed_flags & FIF_FCSFAIL)
		brcms_dbg_info(core, "FIF_FCSFAIL\n");
	if (changed_flags & FIF_CONTROL)
		brcms_dbg_info(core, "FIF_CONTROL\n");
	if (changed_flags & FIF_OTHER_BSS)
		brcms_dbg_info(core, "FIF_OTHER_BSS\n");
	if (changed_flags & FIF_PSPOLL)
		brcms_dbg_info(core, "FIF_PSPOLL\n");
	if (changed_flags & FIF_BCN_PRBRESP_PROMISC)
		brcms_dbg_info(core, "FIF_BCN_PRBRESP_PROMISC\n");

	mutex_lock(&wl->lock);
	brcms_c_mac_promisc(wl->wlc, *total_flags);
	mutex_unlock(&wl->lock);
	return;
}

static void brcms_ops_sw_scan_start(struct ieee80211_hw *hw,
				    struct ieee80211_vif *vif,
				    const u8 *mac_addr)
{
	struct brcms_info *wl = hw->priv;
	mutex_lock(&wl->lock);
	brcms_c_scan_start(wl->wlc);
	mutex_unlock(&wl->lock);
	return;
}

static void brcms_ops_sw_scan_complete(struct ieee80211_hw *hw,
				       struct ieee80211_vif *vif)
{
	struct brcms_info *wl = hw->priv;
	mutex_lock(&wl->lock);
	brcms_c_scan_stop(wl->wlc);
	mutex_unlock(&wl->lock);
	return;
}

static int
brcms_ops_conf_tx(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
		  unsigned int link_id, u16 queue,
		  const struct ieee80211_tx_queue_params *params)
{
	struct brcms_info *wl = hw->priv;

	mutex_lock(&wl->lock);
	brcms_c_wme_setparams(wl->wlc, queue, params, true);
	mutex_unlock(&wl->lock);

	return 0;
}

static int
brcms_ops_sta_add(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
	       struct ieee80211_sta *sta)
{
	struct brcms_info *wl = hw->priv;
	struct scb *scb = &wl->wlc->pri_scb;

	brcms_c_init_scb(scb);

	wl->pub->global_ampdu = &(scb->scb_ampdu);
	wl->pub->global_ampdu->max_pdu = 16;

	/*
	 * minstrel_ht initiates addBA on our behalf by calling
	 * ieee80211_start_tx_ba_session()
	 */
	return 0;
}

static int
brcms_ops_ampdu_action(struct ieee80211_hw *hw,
		    struct ieee80211_vif *vif,
		    struct ieee80211_ampdu_params *params)
{
	struct brcms_info *wl = hw->priv;
	struct scb *scb = &wl->wlc->pri_scb;
	int status;
	struct ieee80211_sta *sta = params->sta;
	enum ieee80211_ampdu_mlme_action action = params->action;
	u16 tid = params->tid;

	if (WARN_ON(scb->magic != SCB_MAGIC))
		return -EIDRM;
	switch (action) {
	case IEEE80211_AMPDU_RX_START:
		break;
	case IEEE80211_AMPDU_RX_STOP:
		break;
	case IEEE80211_AMPDU_TX_START:
		/*
		 * brcmsmac_usb: A-MPDU TX needs the LOW's second-level tx status, which is not
		 * plumbed through RPC yet (docs/split-map.md §7). RX aggregation works.
		 */
		(void)status;
		return -EOPNOTSUPP;

	case IEEE80211_AMPDU_TX_STOP_CONT:
	case IEEE80211_AMPDU_TX_STOP_FLUSH:
	case IEEE80211_AMPDU_TX_STOP_FLUSH_CONT:
		mutex_lock(&wl->lock);
		brcms_c_ampdu_flush(wl->wlc, sta, tid);
		mutex_unlock(&wl->lock);
		ieee80211_stop_tx_ba_cb_irqsafe(vif, sta->addr, tid);
		break;
	case IEEE80211_AMPDU_TX_OPERATIONAL:
		/*
		 * BA window size from ADDBA response ('buf_size') defines how
		 * many outstanding MPDUs are allowed for the BA stream by
		 * recipient and traffic class (this is actually unused by the
		 * rest of the driver). 'ampdu_factor' gives maximum AMPDU size.
		 */
		mutex_lock(&wl->lock);
		brcms_c_ampdu_tx_operational(wl->wlc, tid,
			(1 << (IEEE80211_HT_MAX_AMPDU_FACTOR +
			 sta->deflink.ht_cap.ampdu_factor)) - 1);
		mutex_unlock(&wl->lock);
		/* Power save wakeup */
		break;
	default:
		brcms_err(wl->wlc->hw->d11core,
			  "%s: Invalid command, ignoring\n", __func__);
	}

	return 0;
}

static void brcms_ops_rfkill_poll(struct ieee80211_hw *hw)
{
	struct brcms_info *wl = hw->priv;
	bool blocked;

	mutex_lock(&wl->lock);
	blocked = brcms_c_check_radio_disabled(wl->wlc);
	mutex_unlock(&wl->lock);

	wiphy_rfkill_set_hw_state(wl->pub->ieee_hw->wiphy, blocked);
}

static bool brcms_tx_flush_completed(struct brcms_info *wl)
{
	bool result;

	mutex_lock(&wl->lock);
	result = brcms_c_tx_flush_completed(wl->wlc);
	mutex_unlock(&wl->lock);
	return result;
}

static void brcms_ops_flush(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
			    u32 queues, bool drop)
{
	struct brcms_info *wl = hw->priv;
	int ret;

	no_printk("%s: drop = %s\n", __func__, str_true_false(drop));

	ret = wait_event_timeout(wl->tx_flush_wq,
				 brcms_tx_flush_completed(wl),
				 msecs_to_jiffies(BRCMS_FLUSH_TIMEOUT));

	brcms_dbg_mac80211(wl->wlc->hw->d11core,
			   "ret=%d\n", jiffies_to_msecs(ret));
}

static u64 brcms_ops_get_tsf(struct ieee80211_hw *hw, struct ieee80211_vif *vif)
{
	struct brcms_info *wl = hw->priv;
	u64 tsf;

	mutex_lock(&wl->lock);
	tsf = brcms_c_tsf_get(wl->wlc);
	mutex_unlock(&wl->lock);

	return tsf;
}

static void brcms_ops_set_tsf(struct ieee80211_hw *hw,
			   struct ieee80211_vif *vif, u64 tsf)
{
	struct brcms_info *wl = hw->priv;

	mutex_lock(&wl->lock);
	brcms_c_tsf_set(wl->wlc, tsf);
	mutex_unlock(&wl->lock);
}

static int brcms_ops_beacon_set_tim(struct ieee80211_hw *hw,
				 struct ieee80211_sta *sta, bool set)
{
	struct brcms_info *wl = hw->priv;
	struct sk_buff *beacon = NULL;
	u16 tim_offset = 0;

	mutex_lock(&wl->lock);
	if (wl->wlc->vif)
		beacon = ieee80211_beacon_get_tim(hw, wl->wlc->vif,
						  &tim_offset, NULL, 0);
	if (beacon)
		brcms_c_set_new_beacon(wl->wlc, beacon, tim_offset,
				       wl->wlc->vif->bss_conf.dtim_period);
	mutex_unlock(&wl->lock);

	return 0;
}

static const struct ieee80211_ops brcms_ops = {
	.add_chanctx = ieee80211_emulate_add_chanctx,
	.remove_chanctx = ieee80211_emulate_remove_chanctx,
	.change_chanctx = ieee80211_emulate_change_chanctx,
	.switch_vif_chanctx = ieee80211_emulate_switch_vif_chanctx,
	.tx = brcms_ops_tx,
	.wake_tx_queue = ieee80211_handle_wake_tx_queue,
	.start = brcms_ops_start,
	.stop = brcms_ops_stop,
	.add_interface = brcms_ops_add_interface,
	.remove_interface = brcms_ops_remove_interface,
	.config = brcms_ops_config,
	.bss_info_changed = brcms_ops_bss_info_changed,
	.configure_filter = brcms_ops_configure_filter,
	.sw_scan_start = brcms_ops_sw_scan_start,
	.sw_scan_complete = brcms_ops_sw_scan_complete,
	.conf_tx = brcms_ops_conf_tx,
	.sta_add = brcms_ops_sta_add,
	.ampdu_action = brcms_ops_ampdu_action,
	.rfkill_poll = brcms_ops_rfkill_poll,
	.flush = brcms_ops_flush,
	.get_tsf = brcms_ops_get_tsf,
	.set_tsf = brcms_ops_set_tsf,
	.set_tim = brcms_ops_beacon_set_tim,
};

/*
 * Messages from the LOW (async RPC calls) arrive in URB completion context; queue them and
 * handle them in process context under the perimeter lock, as brcmsmac's DPC did.
 */
#define BRCMS_RPCQ_MAX	512

static void brcms_usb_async(void *ctx, u32 id, const u8 *args, u32 len)
{
	struct brcms_info *wl = ctx;
	struct sk_buff *skb;

	if (skb_queue_len(&wl->rpcq) >= BRCMS_RPCQ_MAX) {
		dev_warn_ratelimited(wl->core.dev, "LOW message queue full, dropping id %u\n", id);
		return;
	}
	skb = dev_alloc_skb(4 + len);
	if (!skb)
		return;
	put_unaligned_le32(id, skb_put(skb, 4));
	skb_put_data(skb, args, len);
	skb_queue_tail(&wl->rpcq, skb);
	schedule_work(&wl->dpc_work);
}

static void brcms_dpc_work(struct work_struct *work)
{
	struct brcms_info *wl = container_of(work, struct brcms_info, dpc_work);
	struct sk_buff *skb;
	bool dpc = false;
	u32 id;

	mutex_lock(&wl->lock);
	while ((skb = skb_dequeue(&wl->rpcq))) {
		id = get_unaligned_le32(skb->data);
		skb_pull(skb, 4);

		switch (id) {
		case BRPC_WLC_RECV:
			if (wl->pub->up) {
				brcms_c_rpc_recv(wl->wlc, skb);
				skb = NULL;
			}
			break;
		case BRPC_DOTXSTATUS:
			if (wl->pub->up)
				brcms_c_rpc_txstatus(wl->wlc, skb->data, skb->len);
			break;
		case BRPC_HIGH_DPC:
			/* (macintstatus, ...) */
			if (skb->len >= 4 && wl->pub->up) {
				wl->wlc->macintstatus |= get_unaligned_le32(skb->data) &
							 wl->wlc->macintmask;
				dpc = true;
			}
			break;
		case BRPC_RESET_BMAC_DONE:
		case BRPC_UPDATE_PHY_MODE:
			brcms_dbg_info(wl->wlc->hw->d11core, "LOW event %u\n", id);
			break;
		case BRPC_FATAL_ERROR:
			brcms_err(wl->wlc->hw->d11core, "firmware reported a fatal error\n");
			break;
		default:
			dev_dbg_ratelimited(wl->core.dev, "unhandled LOW message %u (%u bytes)\n",
					    id, skb->len);
			break;
		}
		kfree_skb(skb);
	}

	if (dpc && wl->pub->up)
		brcms_c_dpc(wl->wlc, true);
	mutex_unlock(&wl->lock);
	wake_up(&wl->tx_flush_wq);
}

/*
 * is called in brcms_pci_probe() context, therefore no locking required.
 */
static int ieee_hw_rate_init(struct ieee80211_hw *hw)
{
	struct brcms_info *wl = hw->priv;
	struct brcms_c_info *wlc = wl->wlc;
	struct ieee80211_supported_band *band;
	u16 phy_type;

	hw->wiphy->bands[NL80211_BAND_2GHZ] = NULL;
	hw->wiphy->bands[NL80211_BAND_5GHZ] = NULL;

	phy_type = brcms_c_get_phy_type(wl->wlc, 0);
	if (phy_type == PHY_TYPE_N || phy_type == PHY_TYPE_LCN) {
		band = &wlc->bandstate[BAND_2G_INDEX]->band;
		*band = brcms_band_2GHz_nphy_template;
		if (phy_type == PHY_TYPE_LCN) {
			/* Single stream */
			band->ht_cap.mcs.rx_mask[1] = 0;
			band->ht_cap.mcs.rx_highest = cpu_to_le16(72);
		}
		hw->wiphy->bands[NL80211_BAND_2GHZ] = band;
	} else {
		return -EPERM;
	}

	/* Assume all bands use the same phy.  True for 11n devices. */
	if (wl->pub->_nbands > 1) {
		if (phy_type == PHY_TYPE_N || phy_type == PHY_TYPE_LCN) {
			band = &wlc->bandstate[BAND_5G_INDEX]->band;
			*band = brcms_band_5GHz_nphy_template;
			hw->wiphy->bands[NL80211_BAND_5GHZ] = band;
		} else {
			return -EPERM;
		}
	}
	return 0;
}

/*
 * is called in brcms_pci_probe() context, therefore no locking required.
 */
static int ieee_hw_init(struct ieee80211_hw *hw)
{
	/* brcmsmac_usb: no AMPDU_AGGREGATION; see brcms_ops_ampdu_action() */
	ieee80211_hw_set(hw, SIGNAL_DBM);
	ieee80211_hw_set(hw, REPORTS_TX_ACK_STATUS);
	ieee80211_hw_set(hw, MFP_CAPABLE);

	hw->extra_tx_headroom = brcms_c_get_header_len();
	hw->queues = N_TX_QUEUES;
	hw->max_rates = 2;	/* Primary rate and 1 fallback rate */

	/*
	 * brcmsmac_usb: station only for now (mac80211 adds monitor). AP and IBSS beacon from
	 * templates, which would bypass the allow_tx gate.
	 */
	hw->wiphy->interface_modes = BIT(NL80211_IFTYPE_STATION);

	/*
	 * deactivate sending probe responses by ucude, because this will
	 * cause problems when WPS is used.
	 *
	 * hw->wiphy->flags |= WIPHY_FLAG_AP_PROBE_RESP_OFFLOAD;
	 */

	wiphy_ext_feature_set(hw->wiphy, NL80211_EXT_FEATURE_CQM_RSSI_LIST);

	hw->rate_control_algorithm = "minstrel_ht";

	hw->sta_data_size = 0;
	return ieee_hw_rate_init(hw);
}

/*
 * brcmsmac_usb: attach the HIGH MAC to a USB dongle whose RPC session is up. Called from the
 * transport's init work (process context).
 */
int brcms_usb_attach(struct bmu_dev *bmu, struct device *dev)
{
	struct brcms_info *wl;
	struct ieee80211_hw *hw;
	u8 perm[ETH_ALEN];
	uint err = 0;
	int ret, unit;

	if (!attach_mac80211)
		return 0;

	hw = ieee80211_alloc_hw(sizeof(struct brcms_info), &brcms_ops);
	if (!hw)
		return -ENOMEM;
	SET_IEEE80211_DEV(hw, dev);

	wl = hw->priv;
	memset(wl, 0, sizeof(*wl));
	wl->wiphy = hw->wiphy;
	wl->bmu = bmu;
	wl->core.bmu = bmu;
	wl->core.dev = dev;
	atomic_set(&wl->callbacks, 0);
	init_waitqueue_head(&wl->tx_flush_wq);
	mutex_init(&wl->lock);
	skb_queue_head_init(&wl->txq);
	skb_queue_head_init(&wl->rpcq);
	INIT_WORK(&wl->tx_work, brcms_tx_work);
	INIT_WORK(&wl->dpc_work, brcms_dpc_work);
	bmu->wl = hw;

	unit = n_adapters_found;

	/* common load-time initialization */
	wl->wlc = brcms_c_attach(wl, &wl->core, unit, false, &err);
	if (!wl->wlc) {
		wiphy_err(wl->wiphy, "%s: attach() failed with code %d\n",
			  KBUILD_MODNAME, err);
		ret = -ENODEV;
		goto fail;
	}
	wl->pub = brcms_c_pub(wl->wlc);
	wl->pub->ieee_hw = hw;

	/* register module */
	brcms_c_module_register(wl->pub, "linux", wl, NULL);

	ret = ieee_hw_init(hw);
	if (ret) {
		wiphy_err(wl->wiphy, "wl%d: %s: ieee_hw_init failed!\n", unit,
			  __func__);
		goto fail;
	}

	brcms_c_regd_init(wl->wlc);

	memcpy(perm, &wl->pub->cur_etheraddr, ETH_ALEN);
	if (WARN_ON(!is_valid_ether_addr(perm))) {
		ret = -EINVAL;
		goto fail;
	}
	SET_IEEE80211_PERM_ADDR(hw, perm);

	/* from here on the LOW's messages are ours */
	bmu_set_async_handler(bmu, brcms_usb_async, wl);

	ret = ieee80211_register_hw(hw);
	if (ret) {
		wiphy_err(wl->wiphy, "%s: ieee80211_register_hw failed, status %d\n",
			  __func__, ret);
		goto fail;
	}
	wl->registered = true;

	if (reg_hint && wl->pub->srom_ccode[0] &&
	    regulatory_hint(wl->wiphy, wl->pub->srom_ccode))
		wiphy_err(wl->wiphy, "%s: regulatory hint failed\n", __func__);

	brcms_debugfs_attach(wl->pub);
	brcms_debugfs_create_files(wl->pub);
	n_adapters_found++;
	wiphy_info(wl->wiphy, "registered %s, MAC %pM\n", wiphy_name(wl->wiphy), perm);
	return 0;

fail:
	brcms_usb_detach(bmu);
	return ret;
}

void brcms_usb_detach(struct bmu_dev *bmu)
{
	struct ieee80211_hw *hw = bmu->wl;
	struct brcms_info *wl;

	if (!hw)
		return;
	wl = hw->priv;

	bmu_set_async_handler(bmu, NULL, NULL);
	if (wl->registered) {
		wiphy_rfkill_stop_polling(hw->wiphy);
		ieee80211_unregister_hw(hw);
		wl->registered = false;
	}
	cancel_work_sync(&wl->tx_work);
	cancel_work_sync(&wl->dpc_work);
	skb_queue_purge(&wl->txq);
	skb_queue_purge(&wl->rpcq);

	brcms_free(wl);
	bmu->wl = NULL;
	ieee80211_free_hw(hw);
}

/*
 * precondition: perimeter lock has been acquired
 */
void brcms_txflowcontrol(struct brcms_info *wl, struct brcms_if *wlif,
			 bool state, int prio)
{
	brcms_err(wl->wlc->hw->d11core, "Shouldn't be here %s\n", __func__);
}

/*
 * precondition: perimeter lock has been acquired
 */
void brcms_init(struct brcms_info *wl)
{
	brcms_dbg_info(wl->wlc->hw->d11core, "Initializing wl%d\n",
		       wl->pub->unit);
	brcms_reset(wl);
	brcms_c_init(wl->wlc, wl->mute_tx);
}

/*
 * precondition: perimeter lock has been acquired
 */
uint brcms_reset(struct brcms_info *wl)
{
	brcms_dbg_info(wl->wlc->hw->d11core, "Resetting wl%d\n", wl->pub->unit);
	brcms_c_reset(wl->wlc);

	/* dpc will not be rescheduled */
	wl->resched = false;

	/* inform publicly that interface is down */
	wl->pub->up = false;

	return 0;
}

void brcms_fatal_error(struct brcms_info *wl)
{
	brcms_err(wl->wlc->hw->d11core, "wl%d: fatal error, reinitializing\n",
		  wl->wlc->pub->unit);
	brcms_reset(wl);
	ieee80211_restart_hw(wl->pub->ieee_hw);
}

/*
 * These are interrupt on/off entry points. Disable interrupts
 * during interrupt state transition.
 */
void brcms_intrson(struct brcms_info *wl)
{
	brcms_c_intrson(wl->wlc);
}

u32 brcms_intrsoff(struct brcms_info *wl)
{
	return brcms_c_intrsoff(wl->wlc);
}

void brcms_intrsrestore(struct brcms_info *wl, u32 macintmask)
{
	brcms_c_intrsrestore(wl->wlc, macintmask);
}

/*
 * precondition: perimeter lock has been acquired
 */
int brcms_up(struct brcms_info *wl)
{
	int error = 0;

	if (wl->pub->up)
		return 0;

	error = brcms_c_up(wl->wlc);

	return error;
}

/*
 * precondition: perimeter lock has been acquired
 */
void brcms_down(struct brcms_info *wl)
	__must_hold(&wl->lock)
{
	uint callbacks, ret_val = 0;
	int i;

	/* call common down function */
	ret_val = brcms_c_down(wl->wlc);
	callbacks = atomic_read(&wl->callbacks) - ret_val;

	/* wait for down callbacks to complete */
	mutex_unlock(&wl->lock);

	/* For HIGH_only driver, it's important to actually schedule other work,
	 * not just spin wait since everything runs at schedule level
	 */
	for (i = 0; i < 100 && atomic_read(&wl->callbacks) > callbacks; i++)
		msleep(1);

	mutex_lock(&wl->lock);
}

/*
* precondition: perimeter lock is not acquired
 */
static void _brcms_timer(struct work_struct *work)
{
	struct brcms_timer *t = container_of(work, struct brcms_timer,
					     dly_wrk.work);

	mutex_lock(&t->wl->lock);

	if (t->set) {
		if (t->periodic) {
			atomic_inc(&t->wl->callbacks);
			ieee80211_queue_delayed_work(t->wl->pub->ieee_hw,
						     &t->dly_wrk,
						     msecs_to_jiffies(t->ms));
		} else {
			t->set = false;
		}

		t->fn(t->arg);
	}

	atomic_dec(&t->wl->callbacks);

	mutex_unlock(&t->wl->lock);
}

/*
 * Adds a timer to the list. Caller supplies a timer function.
 * Is called from wlc.
 *
 * precondition: perimeter lock has been acquired
 */
struct brcms_timer *brcms_init_timer(struct brcms_info *wl,
				     void (*fn) (void *arg),
				     void *arg, const char *name)
{
	struct brcms_timer *t;

	t = kzalloc(sizeof(*t), GFP_ATOMIC);
	if (!t)
		return NULL;

	INIT_DELAYED_WORK(&t->dly_wrk, _brcms_timer);
	t->wl = wl;
	t->fn = fn;
	t->arg = arg;
	t->next = wl->timers;
	wl->timers = t;

#ifdef DEBUG
	t->name = kstrdup(name, GFP_ATOMIC);
#endif

	return t;
}

/*
 * adds only the kernel timer since it's going to be more accurate
 * as well as it's easier to make it periodic
 *
 * precondition: perimeter lock has been acquired
 */
void brcms_add_timer(struct brcms_timer *t, uint ms, int periodic)
{
	struct ieee80211_hw *hw = t->wl->pub->ieee_hw;

#ifdef DEBUG
	if (t->set)
		brcms_dbg_info(t->wl->wlc->hw->d11core,
			       "%s: Already set. Name: %s, per %d\n",
			       __func__, t->name, periodic);
#endif
	t->ms = ms;
	t->periodic = (bool) periodic;
	if (!t->set) {
		t->set = true;
		atomic_inc(&t->wl->callbacks);
	}

	ieee80211_queue_delayed_work(hw, &t->dly_wrk, msecs_to_jiffies(ms));
}

/*
 * return true if timer successfully deleted, false if still pending
 *
 * precondition: perimeter lock has been acquired
 */
bool brcms_del_timer(struct brcms_timer *t)
{
	if (t->set) {
		t->set = false;
		if (!cancel_delayed_work(&t->dly_wrk))
			return false;

		atomic_dec(&t->wl->callbacks);
	}

	return true;
}

/*
 * precondition: perimeter lock has been acquired
 */
void brcms_free_timer(struct brcms_timer *t)
{
	struct brcms_info *wl = t->wl;
	struct brcms_timer *tmp;

	/* delete the timer in case it is active */
	brcms_del_timer(t);

	if (wl->timers == t) {
		wl->timers = wl->timers->next;
#ifdef DEBUG
		kfree(t->name);
#endif
		kfree(t);
		return;

	}

	tmp = wl->timers;
	while (tmp) {
		if (tmp->next == t) {
			tmp->next = t->next;
#ifdef DEBUG
			kfree(t->name);
#endif
			kfree(t);
			return;
		}
		tmp = tmp->next;
	}

}

/*
 * precondition: perimeter lock has been acquired
 */
bool brcms_rfkill_set_hw_state(struct brcms_info *wl)
	__must_hold(&wl->lock)
{
	bool blocked = brcms_c_check_radio_disabled(wl->wlc);

	mutex_unlock(&wl->lock);
	wiphy_rfkill_set_hw_state(wl->pub->ieee_hw->wiphy, blocked);
	if (blocked)
		wiphy_rfkill_start_polling(wl->pub->ieee_hw->wiphy);
	mutex_lock(&wl->lock);
	return blocked;
}
