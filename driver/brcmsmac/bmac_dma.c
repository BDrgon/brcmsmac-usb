// SPDX-License-Identifier: ISC
/*
 * brcmsmac_usb — host emulation of brcmsmac's TX DMA rings (docs/split-map.md §2).
 *
 * There are no descriptor rings on the host: dma_txfast() hands the frame (d11txh + PLCP +
 * 802.11 frame, exactly brcmsmac's DMA buffer) to the firmware with BMAC_TXFIFO (61) and keeps
 * the skb on a per-FIFO pending list until the matching DOTXSTATUS arrives; dma_getnexttxp()
 * then pops it, as on real hardware where tx status comes back in FIFO order. txavail gives
 * mac80211 flow control. Receive needs no ring: frames arrive as WLC_RECV messages.
 *
 * All functions run with the wl mutex held.
 */
#include <linux/slab.h>
#include <linux/module.h>
#include <net/mac80211.h>

#include "../bmu.h"
#include "types.h"
#include "d11.h"
#include "pub.h"
#include "main.h"
#include "dma.h"
#include "bmac.h"

/*
 * Transmission is off by default until the TX path is verified (PLAN phase 6). With it off,
 * frames are completed back to mac80211 as not acknowledged and never reach the air.
 */
bool bmac_allow_tx;
module_param_named(allow_tx, bmac_allow_tx, bool, 0644);
MODULE_PARM_DESC(allow_tx, "let frames reach the radio (default off until the TX path is verified)");

struct bmac_txq {
	struct dma_pub pub;		/* first: brcmsmac only sees this */
	struct brcms_c_info *wlc;
	uint fifo;
	uint ntxd;
	struct sk_buff_head pending;	/* given to the firmware, awaiting tx status */
	struct sk_buff_head dropped_q;	/* not sent; completed by bmac_txq_complete_dropped() */
	unsigned long sent, dropped;
};

static inline struct bmac_txq *to_txq(struct dma_pub *pub)
{
	return container_of(pub, struct bmac_txq, pub);
}

struct dma_pub *bmac_txq_attach(struct brcms_c_info *wlc, uint fifo, uint ntxd)
{
	struct bmac_txq *q = kzalloc(sizeof(*q), GFP_KERNEL);

	if (!q)
		return NULL;
	q->wlc = wlc;
	q->fifo = fifo;
	q->ntxd = ntxd;
	q->pub.txavail = ntxd;
	skb_queue_head_init(&q->pending);
	skb_queue_head_init(&q->dropped_q);
	return &q->pub;
}

void dma_detach(struct dma_pub *pub)
{
	if (!pub)
		return;
	dma_txreclaim(pub, DMA_RANGE_ALL);
	kfree(to_txq(pub));
}

/*
 * BMAC_TXFIFO (61): fifo, commit, frameid, txpktpend, 0, 0, len, then the buffer padded to 4.
 * Words 2–5 are taken from the vendor driver's management frames (0xffff, 1, 0, 0;
 * docs/protocol.md §7.1); their meaning for data frames is still to be confirmed.
 */
int dma_txfast(struct brcms_c_info *wlc, struct dma_pub *pub, struct sk_buff *p)
{
	struct bmac_txq *q = to_txq(pub);
	struct bmac_core *c = wlc->hw->d11core;
	u32 n = ALIGN(p->len, 4);
	u8 *a;
	int ret;

	if (!pub->txavail)
		return -ENOSPC;

	/*
	 * The caller still uses p after we return, so frames that don't go out are parked and
	 * completed later by bmac_txq_complete_dropped().
	 */
	if (!bmac_allow_tx) {
		q->dropped++;
		__skb_queue_tail(&q->dropped_q, p);
		return 0;
	}

	a = kzalloc(28 + n, GFP_KERNEL);
	if (!a)
		return -ENOMEM;
	put_unaligned_le32(q->fifo, a);
	put_unaligned_le32(1, a + 4);		/* commit */
	put_unaligned_le32(0xffff, a + 8);
	put_unaligned_le32(1, a + 12);
	put_unaligned_le32(0, a + 16);
	put_unaligned_le32(0, a + 20);
	put_unaligned_le32(p->len, a + 24);
	memcpy(a + 28, p->data, p->len);
	ret = bmac_call_buf(c, BRPC_TXFIFO, a, 28 + n);
	kfree(a);
	if (ret) {
		q->dropped++;
		__skb_queue_tail(&q->dropped_q, p);
		return 0;	/* reported as not acknowledged; don't stall the queue */
	}
	__skb_queue_tail(&q->pending, p);
	pub->txavail--;
	q->sent++;
	return 0;
}

/* Report frames that never reached the firmware to mac80211 as not acknowledged. */
void bmac_txq_complete_dropped(struct brcms_c_info *wlc)
{
	struct sk_buff *p;
	uint i;

	for (i = 0; i < NFIFO; i++) {
		struct dma_pub *pub = wlc->hw->di[i];

		if (!pub)
			continue;
		while ((p = __skb_dequeue(&to_txq(pub)->dropped_q))) {
			struct ieee80211_tx_info *info = IEEE80211_SKB_CB(p);

			skb_pull(p, D11_PHY_HDR_LEN + D11_TXH_LEN);
			ieee80211_tx_info_clear_status(info);
			ieee80211_tx_status_ni(wlc->pub->ieee_hw, p);
		}
	}
}

struct sk_buff *dma_getnexttxp(struct dma_pub *pub, enum txd_range range)
{
	struct bmac_txq *q = to_txq(pub);
	struct sk_buff *p = __skb_dequeue(&q->pending);

	if (p)
		pub->txavail++;
	return p;
}

int dma_txpending(struct dma_pub *pub)
{
	return skb_queue_len(&to_txq(pub)->pending);
}

void dma_txreclaim(struct dma_pub *pub, enum txd_range range)
{
	struct sk_buff *p;

	while ((p = dma_getnexttxp(pub, range)))
		brcmu_pkt_buf_free_skb(p);
	while ((p = __skb_dequeue(&to_txq(pub)->dropped_q)))
		brcmu_pkt_buf_free_skb(p);
}

void dma_walk_packets(struct dma_pub *pub, void (*callback_fnc)(void *pkt, void *arg_a),
		      void *arg_a)
{
	struct sk_buff *p;

	skb_queue_walk(&to_txq(pub)->pending, p)
		callback_fnc(p, arg_a);
}

void dma_kick_tx(struct dma_pub *pub)
{
}

void dma_rxreclaim(struct dma_pub *pub)
{
}

void dma_counterreset(struct dma_pub *pub)
{
}
