// SPDX-License-Identifier: ISC
/*
 * brcmsmac_usb — RPC core: session handshake, one-way and synchronous calls, dispatch of
 * dongle→host messages, trace ring. See docs/protocol.md §4.
 */
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/ctype.h>
#include "bmu.h"

/* ------------------------------------------------------------------------------------------ */
/* trace ring */

void bmu_trace(struct bmu_dev *d, u8 dir, u8 kind, u16 seq, u32 id, const void *data, u32 len)
{
	struct bmu_trace_ent *e;
	unsigned long flags;

	spin_lock_irqsave(&d->trace_lock, flags);
	e = &d->trace[d->trace_head++ % BMU_TRACE_LEN];
	e->t = ktime_get();
	e->dir = dir;
	e->kind = kind;
	e->seq = seq;
	e->id = id;
	e->len = len;
	memset(e->bytes, 0, sizeof(e->bytes));
	if (data)
		memcpy(e->bytes, data, min_t(u32, len, BMU_TRACE_BYTES));
	spin_unlock_irqrestore(&d->trace_lock, flags);
}

/* ------------------------------------------------------------------------------------------ */
/* dongle → host */

void bmu_set_async_handler(struct bmu_dev *d, bmu_async_fn fn, void *ctx)
{
	unsigned long flags;

	spin_lock_irqsave(&d->lock, flags);
	d->async_ctx = ctx;
	d->async_fn = fn;
	spin_unlock_irqrestore(&d->lock, flags);
}

/* Split one bulk IN transfer into messages (several may be concatenated) and dispatch them. */
void bmu_rx_frames(struct bmu_dev *d, const u8 *buf, u32 len)
{
	u32 off = 0;

	while (off + 8 <= len) {
		u32 n = get_unaligned_le32(buf + off);
		const u8 *msg = buf + off + 4;
		u32 hdr, kind;

		if (n < 4 || off + 4 + n > len) {
			d->stats.rx_bad_frames++;
			dev_dbg_ratelimited(&d->intf->dev, "bad frame length %u at %u/%u\n", n, off, len);
			return;
		}
		hdr = get_unaligned_le32(msg);
		kind = hdr >> 24;
		d->stats.rx_msgs++;
		d->stats.rx_bytes += n + 4;

		if (kind == BMU_KIND_MGMT) {
			u32 m = min_t(u32, n - 4, sizeof(d->mgmt_reply));

			bmu_trace(d, 'I', kind, hdr & 0xffff, 0, msg + 4, n - 4);
			memcpy(d->mgmt_reply, msg + 4, m);
			d->mgmt_len = m;
			complete(&d->mgmt_done);
		} else if (kind == BMU_KIND_CALL && n >= 8) {
			u32 id = get_unaligned_le32(msg + 4);
			bmu_async_fn fn;
			void *ctx;

			d->stats.async_by_id[id & 0xff]++;
			bmu_trace(d, 'I', kind, hdr & 0xffff, id, msg + 8, n - 8);
			unsigned long flags;

			spin_lock_irqsave(&d->lock, flags);
			fn = d->async_fn;
			ctx = d->async_ctx;
			spin_unlock_irqrestore(&d->lock, flags);
			if (fn)
				fn(ctx, id, msg + 8, n - 8);
		} else {
			d->stats.rx_bad_frames++;
		}
		off += 4 + n;
	}
}

/* ------------------------------------------------------------------------------------------ */
/* session */

static int bmu_mgmt(struct bmu_dev *d, u32 action, u32 word3, u32 *reply, int nwords)
{
	__le32 p[3] = { cpu_to_le32(action), cpu_to_le32(d->fw->verword), cpu_to_le32(word3) };
	long left;
	int ret, i;

	reinit_completion(&d->mgmt_done);
	d->mgmt_len = 0;
	ret = bmu_submit_msg(d, BMU_KIND_MGMT, p, sizeof(p), GFP_KERNEL, NULL, NULL);
	if (ret)
		return ret;
	left = wait_for_completion_timeout(&d->mgmt_done, msecs_to_jiffies(BMU_MGMT_TIMEOUT_MS));
	if (READ_ONCE(d->state) == BMU_ST_GONE)
		return -ENODEV;
	if (!left)
		return -ETIMEDOUT;
	if (d->mgmt_len < nwords * 4)
		return -EPROTO;
	for (i = 0; i < nwords; i++)
		reply[i] = get_unaligned_le32(d->mgmt_reply + 4 * i);
	return 0;
}

int bmu_connect(struct bmu_dev *d)
{
	struct device *dev = &d->intf->dev;
	u32 r[4];
	int ret;

	ret = bmu_mgmt(d, BMU_MGMT_HELLO, 0, r, 4);
	if (ret) {
		dev_err(dev, "HELLO: no reply (%d)\n", ret);
		return ret;
	}
	if (r[0] != BMU_MGMT_HELLO) {
		dev_err(dev, "HELLO: unexpected reply action %u\n", r[0]);
		return -EPROTO;
	}
	ret = bmu_mgmt(d, BMU_MGMT_CONNECT, r[3], r, 3);
	if (ret) {
		dev_err(dev, "CONNECT: no reply (%d)\n", ret);
		return ret;
	}
	if (r[0] == BMU_MGMT_REFUSE) {
		dev_err(dev, "firmware refused the session (status %u%s); is %s the running firmware?\n",
			r[2], r[2] == 3 ? ": version mismatch" : "", d->fw->version);
		return -EPROTO;
	}
	if (r[0] != BMU_MGMT_ACCEPT) {
		dev_err(dev, "CONNECT: unexpected reply action %u\n", r[0]);
		return -EPROTO;
	}
	d->reconnected = r[2] == 2;
	dev_info(dev, "RPC session up (firmware %s%s)\n", d->fw->version,
		 d->reconnected ? ", reconnected" : "");
	return 0;
}

/* ------------------------------------------------------------------------------------------ */
/* host → dongle calls */

/*
 * Register/shared-memory access before BMAC_INIT hangs the whole dongle (docs/protocol.md §8),
 * which on some hosts only a replug fixes. Refuse it outright.
 */
static int bmu_guard(struct bmu_dev *d, u32 id)
{
	switch (id) {
	case BMU_ID_BMAC_INIT:
		WRITE_ONCE(d->bmac_init_sent, true);
		return 0;
	case BMU_ID_REG_READ:
	case BMU_ID_REG_WRITE:
	case BMU_ID_COPYFROM_OBJMEM:
	case BMU_ID_COPYTO_OBJMEM:
	case BMU_ID_READ_SHM:
	case BMU_ID_READ_TSF:
	case BMU_ID_WRITE_SHM:
	case BMU_ID_WRITE_TEMPLATE:
		if (!READ_ONCE(d->bmac_init_sent)) {
			dev_warn_ratelimited(&d->intf->dev,
					     "refusing rpc %u before BMAC_INIT (would hang the dongle)\n",
					     id);
			return -EPERM;
		}
		return 0;
	default:
		return 0;
	}
}

static void *bmu_build(u32 id, const void *args, u32 arglen, gfp_t gfp)
{
	u8 *p = kmalloc(4 + arglen, gfp);

	if (!p)
		return NULL;
	put_unaligned_le32(id, p);
	if (arglen)
		memcpy(p + 4, args, arglen);
	return p;
}

/* One-way call. Safe in atomic context with GFP_ATOMIC. args: LE words / padded bytes. */
int bmu_call(struct bmu_dev *d, u32 id, const void *args, u32 arglen, gfp_t gfp)
{
	void *p;
	int ret;

	if (arglen & 3)
		return -EINVAL;
	ret = bmu_guard(d, id);
	if (ret)
		return ret;
	if (READ_ONCE(d->state) != BMU_ST_UP)
		return -ENODEV;
	p = bmu_build(id, args, arglen, gfp);
	if (!p)
		return -ENOMEM;
	ret = bmu_submit_msg(d, BMU_KIND_CALL, p, 4 + arglen, gfp, NULL, NULL);
	kfree(p);
	return ret;
}

/*
 * Synchronous call: queue the message, wait for it to leave, then read the reply with vendor
 * request 9. Sleeps; calls are serialised. Returns the reply body after the echoed id.
 */
int bmu_sync(struct bmu_dev *d, u32 id, const void *args, u32 arglen,
	     void *ret, u32 retmax, u32 *retlen)
{
	struct device *dev = &d->intf->dev;
	DECLARE_COMPLETION_ONSTACK(sent);
	int status = 0, r, n;
	u32 tp_len, hdr, echo, body;
	void *p;

	might_sleep();
	if (arglen & 3)
		return -EINVAL;
	r = bmu_guard(d, id);
	if (r)
		return r;
	p = bmu_build(id, args, arglen, GFP_KERNEL);
	if (!p)
		return -ENOMEM;

	mutex_lock(&d->sync_mutex);
	if (READ_ONCE(d->state) != BMU_ST_UP && READ_ONCE(d->state) != BMU_ST_CONNECTING) {
		r = -ENODEV;
		goto out;
	}
	d->stats.sync_calls++;
	r = bmu_submit_msg(d, BMU_KIND_SYNC, p, 4 + arglen, GFP_KERNEL, &sent, &status);
	if (r)
		goto out;
	if (!wait_for_completion_timeout(&sent, msecs_to_jiffies(BMU_SYNC_TIMEOUT_MS))) {
		d->stats.sync_timeouts++;
		usb_kill_anchored_urbs(&d->tx_anchor);
		wait_for_completion(&sent);
		bmu_mark_hung(d, "bulk OUT stalled");
		r = -ETIMEDOUT;
		goto out;
	}
	if (status) {
		d->stats.sync_errs++;
		r = status;
		goto out;
	}

	n = usb_control_msg(d->udev, usb_rcvctrlpipe(d->udev, 0), BMU_RPC_RETURN,
			    USB_DIR_IN | USB_TYPE_VENDOR | USB_RECIP_INTERFACE, 0, 0,
			    d->ctrl_buf, BMU_RETURN_WLEN, BMU_SYNC_TIMEOUT_MS);
	if (n < 0) {
		u32 chip;

		d->stats.sync_errs++;
		if (n == -ETIMEDOUT) {
			d->stats.sync_timeouts++;
			if (bmu_getver(d, &chip))
				bmu_mark_hung(d, "no reply to sync call and DL_GETVER");
			else
				dev_err(dev, "rpc %u: no reply (firmware id 0x%x still answers)\n",
					id, chip);
		}
		r = n;
		goto out;
	}
	if (n < 12) {
		r = -EPROTO;
		goto out;
	}
	tp_len = get_unaligned_le32(d->ctrl_buf);
	hdr = get_unaligned_le32(d->ctrl_buf + 4);
	echo = get_unaligned_le32(d->ctrl_buf + 8);
	if (tp_len + 4 > n || tp_len < 8 || hdr >> 24 != BMU_KIND_SYNC || echo != id) {
		dev_err(dev, "rpc %u: bad reply (len %u/%d, hdr 0x%08x, echo %u)\n",
			id, tp_len, n, hdr, echo);
		d->stats.sync_errs++;
		r = -EPROTO;
		goto out;
	}
	body = tp_len - 8;
	bmu_trace(d, 'R', BMU_KIND_SYNC, hdr & 0xffff, id, d->ctrl_buf + 12, body);
	if (ret)
		memcpy(ret, d->ctrl_buf + 12, min(body, retmax));
	if (retlen)
		*retlen = body;
	r = 0;
out:
	mutex_unlock(&d->sync_mutex);
	kfree(p);
	return r;
}

/* ------------------------------------------------------------------------------------------ */
/* identity: read-only calls that are safe before BMAC_INIT (docs/protocol.md §6) */

int bmu_identify(struct bmu_dev *d)
{
	struct device *dev = &d->intf->dev;
	u8 *buf;
	u32 len, i;
	int ret;

	buf = kmalloc(BMU_RETURN_WLEN, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	ret = bmu_sync(d, BMU_ID_REVINFO_GET, NULL, 0, buf, BMU_RETURN_WLEN, &len);
	if (ret)
		goto out;
	d->revinfo_len = min_t(u32, len, sizeof(d->revinfo));
	memcpy(d->revinfo, buf, d->revinfo_len);

	ret = bmu_sync(d, BMU_ID_STATE_GET, NULL, 0, buf, BMU_RETURN_WLEN, &len);
	if (ret)
		goto out;

	for (i = 0; i < 6; i++) {
		__le32 fifo = cpu_to_le32(i);

		ret = bmu_sync(d, BMU_ID_XMTFIFO_SZ_GET, &fifo, 4, buf, BMU_RETURN_WLEN, &len);
		if (ret)
			goto out;
		d->fifo_sz[i] = len >= 4 ? get_unaligned_le32(buf) : 0;
	}

	ret = bmu_sync(d, BMU_ID_COPYFROM_VARS, NULL, 0, buf, BMU_RETURN_WLEN, &len);
	if (ret)
		goto out;
	if (len > 4) {
		u32 n = min_t(u32, len - 4, sizeof(d->nvram_head) - 1);

		memcpy(d->nvram_head, buf + 4, n);
		d->nvram_head[n] = 0;
		for (i = 0; i < n; i++)
			if (!d->nvram_head[i])
				d->nvram_head[i] = ' ';
			else if (!isprint(d->nvram_head[i]))
				d->nvram_head[i] = '.';
	}

	ret = bmu_sync(d, BMU_ID_HW_ETHERADDR, NULL, 0, buf, BMU_RETURN_WLEN, &len);
	if (ret)
		goto out;
	if (len >= 6)
		memcpy(d->macaddr, buf, 6);

	dev_info(dev, "chip 0x%04x (vendor 0x%04x), tx fifos %u/%u/%u/%u/%u/%u, MAC %pM\n",
		 d->revinfo_len >= 28 ? get_unaligned_le32(d->revinfo + 24) : 0,
		 d->revinfo_len >= 4 ? get_unaligned_le32(d->revinfo) : 0,
		 d->fifo_sz[0], d->fifo_sz[1], d->fifo_sz[2], d->fifo_sz[3], d->fifo_sz[4],
		 d->fifo_sz[5], d->macaddr);
	dev_info(dev, "NVRAM: %s ...\n", d->nvram_head);
out:
	if (ret)
		dev_err(dev, "identity calls failed (%d)\n", ret);
	kfree(buf);
	return ret;
}
