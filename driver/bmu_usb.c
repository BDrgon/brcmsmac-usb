// SPDX-License-Identifier: ISC
/*
 * brcmsmac_usb — USB layer: probe/disconnect, firmware download (RDL), transport framing,
 * bulk IN URB pool and recovery. See docs/protocol.md §1–3.
 */
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/firmware.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include "bmu.h"
#include "brcmsmac/bmac.h"

static char *fw_version = "5.100.148.5";
module_param(fw_version, charp, 0444);
MODULE_PARM_DESC(fw_version, "dongle firmware version to load (must exist in the driver's table)");

bool bmu_param_allow_reset = true;
module_param_named(allow_reset, bmu_param_allow_reset, bool, 0644);
MODULE_PARM_DESC(allow_reset, "attempt a USB port reset when the dongle stops answering");

static bool identify = true;
module_param(identify, bool, 0444);
MODULE_PARM_DESC(identify, "read revinfo/NVRAM/MAC after connecting and log them");

static const struct bmu_fw bmu_fw_table[] = {
	{ "5.100.148.5", 0x05649405, "brcm/bcm4322-bmac-5.100.148.5.trx" },
	{ "5.60.180.11", 0x053cb40b, "brcm/bcm4322-bmac-5.60.180.11.trx" },
};

static const struct usb_device_id bmu_table[] = {
	{ USB_DEVICE_AND_INTERFACE_INFO(0x0846, 0x9020, 0xff, 0x02, 0xff) },	/* WNA3100 v1 */
	{ }
};
MODULE_DEVICE_TABLE(usb, bmu_table);

/* ------------------------------------------------------------------------------------------ */
/* vendor requests */

int bmu_vreq_in(struct bmu_dev *d, u8 req, void *buf, u16 len, int timeout_ms)
{
	u8 *b = kmalloc(len, GFP_KERNEL);	/* DMA-safe, and independent of sync calls */
	int ret;

	if (!b)
		return -ENOMEM;
	ret = usb_control_msg(d->udev, usb_rcvctrlpipe(d->udev, 0), req,
			      USB_DIR_IN | USB_TYPE_VENDOR | USB_RECIP_INTERFACE,
			      0, 0, b, len, timeout_ms);
	if (ret > 0 && buf)
		memcpy(buf, b, ret);
	kfree(b);
	return ret;
}

int bmu_getver(struct bmu_dev *d, u32 *chip)
{
	u8 b[8];
	int ret = bmu_vreq_in(d, BMU_DL_GETVER, b, sizeof(b), 1000);

	if (ret < 4)
		return ret < 0 ? ret : -EIO;
	*chip = get_unaligned_le32(b);
	return 0;
}

static int bmu_rdl_state(struct bmu_dev *d, u32 *st, u32 *bytes)
{
	u8 b[8];
	int ret = bmu_vreq_in(d, BMU_DL_GETSTATE, b, sizeof(b), 1000);

	if (ret < 8)
		return ret < 0 ? ret : -EIO;
	*st = get_unaligned_le32(b);
	*bytes = get_unaligned_le32(b + 4);
	return 0;
}

/* Download a TRX image to the boot ROM and start it. */
int bmu_download(struct bmu_dev *d, const u8 *img, size_t len)
{
	struct device *dev = &d->intf->dev;
	unsigned long deadline;
	u32 st, got, sent = 0;
	u8 b[8], *chunk;
	int ret, actual;

	ret = bmu_vreq_in(d, BMU_DL_START, b, sizeof(b), 1000);
	if (ret < 8)
		return ret < 0 ? ret : -EIO;
	st = get_unaligned_le32(b);
	got = get_unaligned_le32(b + 4);
	if (st != BMU_RDL_WAITING) {
		dev_err(dev, "boot ROM not waiting for an image (state %u)\n", st);
		return -EBUSY;
	}

	chunk = kmalloc(BMU_RDL_CHUNK, GFP_KERNEL);
	if (!chunk)
		return -ENOMEM;

	deadline = jiffies + msecs_to_jiffies(20000);
	while (got != len) {
		if (got == sent && sent < len) {
			u32 n = min_t(u32, BMU_RDL_CHUNK, len - sent);

			memcpy(chunk, img + sent, n);
			ret = usb_bulk_msg(d->udev, d->pipe_out, chunk, n, &actual, 5000);
			if (ret)
				goto out;
			sent += n;
		}
		ret = bmu_rdl_state(d, &st, &got);
		if (ret)
			goto out;
		if (st == 2 || st == 3 || st >= 5) {
			dev_err(dev, "download failed: state %u at %u/%zu bytes\n", st, got, len);
			ret = -EIO;
			goto out;
		}
		if (time_after(jiffies, deadline)) {
			dev_err(dev, "download stalled at %u/%zu bytes\n", got, len);
			ret = -ETIMEDOUT;
			goto out;
		}
	}

	for (ret = 0; ret < 50; ret++) {
		if (bmu_rdl_state(d, &st, &got))
			break;
		if (st == BMU_RDL_RUNNABLE)
			break;
		msleep(100);
	}
	if (st != BMU_RDL_RUNNABLE) {
		dev_err(dev, "image not runnable (state %u)\n", st);
		ret = -EIO;
		goto out;
	}
	ret = bmu_vreq_in(d, BMU_DL_GO, b, sizeof(b), 1000);
	ret = ret < 0 ? ret : 0;	/* some ROMs drop the reply; caller re-checks GETVER */
out:
	kfree(chunk);
	return ret;
}

/* ------------------------------------------------------------------------------------------ */
/* transport: host → dongle */

struct bmu_txctx {
	struct bmu_dev *d;
	struct completion *done;
	int *status;
};

static void bmu_tx_complete(struct urb *urb)
{
	struct bmu_txctx *c = urb->context;

	if (urb->status) {
		c->d->stats.tx_errs++;
		if (urb->status != -ENOENT && urb->status != -ECONNRESET &&
		    urb->status != -ESHUTDOWN && urb->status != -ENODEV)
			dev_warn_ratelimited(&c->d->intf->dev, "bulk OUT error %d\n", urb->status);
	}
	if (c->status)
		*c->status = urb->status;
	if (c->done)
		complete(c->done);
	kfree(urb->transfer_buffer);
	kfree(c);
}

/*
 * Frame and queue one message. The sequence number is allocated and the URB submitted under
 * d->lock, so messages reach the endpoint in sequence order whatever the calling context.
 */
int bmu_submit_msg(struct bmu_dev *d, u8 kind, const void *payload, u32 len, gfp_t gfp,
		   struct completion *done, int *status)
{
	struct bmu_txctx *c;
	struct urb *urb;
	unsigned long flags;
	u8 *buf;
	u16 seq;
	u32 id;
	int ret;

	if (len + 8 > BMU_MSG_MAX + 8)
		return -EMSGSIZE;
	urb = usb_alloc_urb(0, gfp);
	buf = kmalloc(len + 8, gfp);
	c = kmalloc(sizeof(*c), gfp);
	if (!urb || !buf || !c) {
		ret = -ENOMEM;
		goto err;
	}
	c->d = d;
	c->done = done;
	c->status = status;
	put_unaligned_le32(len + 4, buf);
	memcpy(buf + 8, payload, len);
	id = (kind != BMU_KIND_MGMT && len >= 4) ? get_unaligned_le32(payload) : 0;

	spin_lock_irqsave(&d->lock, flags);
	if (d->state == BMU_ST_GONE || (d->state == BMU_ST_HUNG && kind != BMU_KIND_MGMT)) {
		spin_unlock_irqrestore(&d->lock, flags);
		ret = -ENODEV;
		goto err;
	}
	seq = kind == BMU_KIND_MGMT ? 0 : d->seq++;
	put_unaligned_le32((u32)kind << 24 | (u32)d->session << 16 | seq, buf + 4);
	usb_fill_bulk_urb(urb, d->udev, d->pipe_out, buf, len + 8, bmu_tx_complete, c);
	usb_anchor_urb(urb, &d->tx_anchor);
	ret = usb_submit_urb(urb, GFP_ATOMIC);
	if (ret) {
		usb_unanchor_urb(urb);
	} else {
		d->stats.tx_msgs++;
		d->stats.tx_bytes += len + 8;
		bmu_trace(d, 'O', kind, seq, id, payload, len);
	}
	spin_unlock_irqrestore(&d->lock, flags);
	usb_free_urb(urb);	/* the anchor holds a reference until completion */
	if (ret) {
		kfree(buf);
		kfree(c);
		d->stats.tx_errs++;
	}
	return ret;
err:
	usb_free_urb(urb);
	kfree(buf);
	kfree(c);
	return ret;
}

/* ------------------------------------------------------------------------------------------ */
/* transport: dongle → host (bulk IN) */

static void bmu_rx_complete(struct urb *urb)
{
	struct bmu_dev *d = urb->context;
	int ret;

	switch (urb->status) {
	case 0:
		d->stats.rx_xfers++;
		bmu_rx_frames(d, urb->transfer_buffer, urb->actual_length);
		break;
	case -ENOENT:
	case -ECONNRESET:
	case -ESHUTDOWN:
	case -ENODEV:
		return;		/* killed or device gone */
	default:
		d->stats.rx_errs++;
		dev_dbg_ratelimited(&d->intf->dev, "bulk IN status %d\n", urb->status);
		if (urb->status == -EPROTO || urb->status == -EILSEQ || urb->status == -ETIME) {
			/* typically the device just fell off the bus; don't spin on it */
			if (d->stats.rx_errs > 64 && d->stats.rx_errs > d->stats.rx_xfers)
				return;
		}
		break;
	}

	if (READ_ONCE(d->state) == BMU_ST_GONE)
		return;
	usb_anchor_urb(urb, &d->rx_anchor);
	ret = usb_submit_urb(urb, GFP_ATOMIC);
	if (ret) {
		usb_unanchor_urb(urb);
		if (ret != -ENODEV && ret != -EPERM)
			dev_warn_ratelimited(&d->intf->dev, "bulk IN resubmit failed: %d\n", ret);
	}
}

int bmu_rx_start(struct bmu_dev *d)
{
	int i, ret;

	for (i = 0; i < BMU_RX_URBS; i++) {
		struct urb *urb = d->rx_urb[i];

		if (!urb) {
			urb = usb_alloc_urb(0, GFP_KERNEL);
			if (!urb)
				return -ENOMEM;
			urb->transfer_buffer = kmalloc(BMU_RX_BUFSZ, GFP_KERNEL);
			if (!urb->transfer_buffer) {
				usb_free_urb(urb);
				return -ENOMEM;
			}
			d->rx_urb[i] = urb;
		}
		usb_fill_bulk_urb(urb, d->udev, d->pipe_in, urb->transfer_buffer, BMU_RX_BUFSZ,
				  bmu_rx_complete, d);
		usb_anchor_urb(urb, &d->rx_anchor);
		ret = usb_submit_urb(urb, GFP_KERNEL);
		if (ret) {
			usb_unanchor_urb(urb);
			return ret;
		}
	}
	return 0;
}

void bmu_rx_stop(struct bmu_dev *d)
{
	usb_kill_anchored_urbs(&d->rx_anchor);
}

static void bmu_rx_free(struct bmu_dev *d)
{
	int i;

	for (i = 0; i < BMU_RX_URBS; i++) {
		if (!d->rx_urb[i])
			continue;
		kfree(d->rx_urb[i]->transfer_buffer);
		usb_free_urb(d->rx_urb[i]);
		d->rx_urb[i] = NULL;
	}
}

/* ------------------------------------------------------------------------------------------ */
/* bring-up */

/* Move to a new state unless the device is going away (or hung, for non-recovery callers). */
static bool bmu_set_state(struct bmu_dev *d, enum bmu_state st)
{
	unsigned long flags;
	bool ok;

	spin_lock_irqsave(&d->lock, flags);
	ok = d->state != BMU_ST_GONE && d->state != BMU_ST_HUNG;
	if (ok)
		d->state = st;
	spin_unlock_irqrestore(&d->lock, flags);
	return ok;
}

static const struct bmu_fw *bmu_find_fw(const char *ver)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(bmu_fw_table); i++)
		if (!strcmp(bmu_fw_table[i].version, ver))
			return &bmu_fw_table[i];
	return NULL;
}

static int bmu_check_trx(struct bmu_dev *d, const struct firmware *fw)
{
	if (fw->size < 28 || memcmp(fw->data, "HDR0", 4)) {
		dev_err(&d->intf->dev, "%s is not a TRX image\n", d->fw->file);
		return -EINVAL;
	}
	if (get_unaligned_le32(fw->data + 4) != fw->size) {
		dev_err(&d->intf->dev, "%s: TRX length %u != file size %zu\n", d->fw->file,
			get_unaligned_le32(fw->data + 4), fw->size);
		return -EINVAL;
	}
	return 0;
}

static void bmu_init_work(struct work_struct *work)
{
	struct bmu_dev *d = container_of(work, struct bmu_dev, init_work);
	struct device *dev = &d->intf->dev;
	const struct firmware *fw = NULL;
	u32 chip;
	int ret;

	ret = bmu_getver(d, &chip);
	if (ret) {
		dev_err(dev, "DL_GETVER failed (%d); dongle unresponsive\n", ret);
		bmu_mark_hung(d, "no answer to DL_GETVER at probe");
		return;
	}

	if (chip == BMU_CHIP_ROM) {
		if (!bmu_set_state(d, BMU_ST_DOWNLOADING))
			return;
		ret = request_firmware(&fw, d->fw->file, dev);
		if (ret) {
			dev_err(dev, "firmware %s not found (%d); extract it from your vendor package (see README)\n",
				d->fw->file, ret);
			return;
		}
		ret = bmu_check_trx(d, fw);
		if (!ret)
			ret = bmu_download(d, fw->data, fw->size);
		release_firmware(fw);
		if (ret) {
			dev_err(dev, "firmware download failed (%d)\n", ret);
			return;
		}
		msleep(500);
		ret = bmu_getver(d, &chip);
		if (ret || chip != BMU_CHIP_RUNNING) {
			dev_err(dev, "firmware didn't start (ret %d, id 0x%x)\n", ret, chip);
			return;
		}
		dev_info(dev, "firmware %s downloaded and running\n", d->fw->version);
	} else if (chip == BMU_CHIP_RUNNING) {
		dev_info(dev, "firmware already running; reconnecting\n");
	} else {
		dev_err(dev, "unexpected DL_GETVER id 0x%x\n", chip);
		return;
	}

	bmu_vreq_in(d, BMU_DL_RESETCFG, NULL, 8, 1000);

	if (!bmu_set_state(d, BMU_ST_CONNECTING))
		return;
	ret = bmu_rx_start(d);
	if (ret) {
		dev_err(dev, "bulk IN start failed (%d)\n", ret);
		return;
	}
	ret = bmu_connect(d);
	if (ret) {
		dev_err(dev, "RPC session failed (%d)\n", ret);
		bmu_rx_stop(d);
		return;
	}
	if (!bmu_set_state(d, BMU_ST_UP))
		return;
	if (identify)
		bmu_identify(d);

	/* bring up the brcmsmac HIGH MAC on top of the session (docs/split-map.md) */
	ret = brcms_usb_attach(d, dev);
	if (ret)
		dev_err(dev, "mac80211 attach failed (%d); transport stays up for debugging\n", ret);
}

/*
 * Hang recovery: a USB port reset re-probes the device (we have no pre/post_reset hooks), so the
 * attempt budget is module-wide: at most 3 resets per 10 minutes.
 */
static DEFINE_SPINLOCK(bmu_reset_lock);
static unsigned long bmu_reset_window;
static unsigned int bmu_reset_count;

static bool bmu_reset_allowed(void)
{
	bool ok;

	spin_lock(&bmu_reset_lock);
	if (!bmu_reset_window || time_after(jiffies, bmu_reset_window + 600 * HZ)) {
		bmu_reset_window = jiffies;
		bmu_reset_count = 0;
	}
	ok = bmu_reset_count < 3;
	if (ok)
		bmu_reset_count++;
	spin_unlock(&bmu_reset_lock);
	return ok;
}

static void bmu_recover_work(struct work_struct *work)
{
	struct bmu_dev *d = container_of(to_delayed_work(work), struct bmu_dev, recover_work);

	if (READ_ONCE(d->state) != BMU_ST_HUNG || !bmu_param_allow_reset)
		return;
	if (!bmu_reset_allowed()) {
		dev_err(&d->intf->dev, "dongle hung; reset budget used up — replug it\n");
		return;
	}
	d->stats.recoveries++;
	dev_warn(&d->intf->dev, "dongle hung; resetting USB port\n");
	usb_queue_reset_device(d->intf);
}

void bmu_mark_hung(struct bmu_dev *d, const char *why)
{
	unsigned long flags;

	spin_lock_irqsave(&d->lock, flags);
	if (d->state == BMU_ST_GONE || d->state == BMU_ST_HUNG) {
		spin_unlock_irqrestore(&d->lock, flags);
		return;
	}
	d->state = BMU_ST_HUNG;
	spin_unlock_irqrestore(&d->lock, flags);
	dev_err(&d->intf->dev, "dongle not answering: %s\n", why);
	usb_kill_anchored_urbs(&d->tx_anchor);
	if (bmu_param_allow_reset)
		schedule_delayed_work(&d->recover_work, msecs_to_jiffies(1000));
}

/* ------------------------------------------------------------------------------------------ */

static int bmu_probe(struct usb_interface *intf, const struct usb_device_id *id)
{
	struct usb_device *udev = interface_to_usbdev(intf);
	struct bmu_dev *d;

	d = kzalloc(sizeof(*d), GFP_KERNEL);
	if (!d)
		return -ENOMEM;
	d->ctrl_buf = kmalloc(BMU_RETURN_WLEN, GFP_KERNEL);
	if (!d->ctrl_buf) {
		kfree(d);
		return -ENOMEM;
	}
	d->fw = bmu_find_fw(fw_version);
	if (!d->fw) {
		dev_err(&intf->dev, "unknown firmware version '%s'\n", fw_version);
		kfree(d->ctrl_buf);
		kfree(d);
		return -EINVAL;
	}
	d->udev = usb_get_dev(udev);
	d->intf = intf;
	d->pipe_out = usb_sndbulkpipe(udev, BMU_EP_OUT);
	d->pipe_in = usb_rcvbulkpipe(udev, BMU_EP_IN);
	d->session = 0x69;
	d->state = BMU_ST_PROBING;
	spin_lock_init(&d->lock);
	spin_lock_init(&d->trace_lock);
	mutex_init(&d->sync_mutex);
	init_completion(&d->mgmt_done);
	init_usb_anchor(&d->rx_anchor);
	init_usb_anchor(&d->tx_anchor);
	INIT_WORK(&d->init_work, bmu_init_work);
	INIT_DELAYED_WORK(&d->recover_work, bmu_recover_work);
	usb_set_intfdata(intf, d);

	bmu_debugfs_init(d);
	schedule_work(&d->init_work);
	return 0;
}

static void bmu_disconnect(struct usb_interface *intf)
{
	struct bmu_dev *d = usb_get_intfdata(intf);
	unsigned long flags;

	if (!d)
		return;
	bmu_debugfs_exit(d);		/* waits for in-flight debugfs ops */

	/*
	 * Unplugged: fail every call at once. Otherwise (rmmod, unbind, reset) keep the session
	 * so the HIGH MAC can take the firmware down cleanly.
	 */
	if (interface_to_usbdev(intf)->state == USB_STATE_NOTATTACHED) {
		spin_lock_irqsave(&d->lock, flags);
		d->state = BMU_ST_GONE;
		spin_unlock_irqrestore(&d->lock, flags);
		complete_all(&d->mgmt_done);
	}
	cancel_work_sync(&d->init_work);	/* attach runs from here */
	brcms_usb_detach(d);

	spin_lock_irqsave(&d->lock, flags);
	d->state = BMU_ST_GONE;
	spin_unlock_irqrestore(&d->lock, flags);
	complete_all(&d->mgmt_done);
	cancel_delayed_work_sync(&d->recover_work);
	WRITE_ONCE(d->state, BMU_ST_GONE);
	usb_kill_anchored_urbs(&d->rx_anchor);
	usb_kill_anchored_urbs(&d->tx_anchor);
	mutex_lock(&d->sync_mutex);	/* wait out a sync call in progress */
	mutex_unlock(&d->sync_mutex);
	usb_set_intfdata(intf, NULL);
	bmu_rx_free(d);
	usb_put_dev(d->udev);
	kfree(d->ctrl_buf);
	kfree(d);
	dev_info(&intf->dev, "disconnected\n");
}

static struct usb_driver bmu_driver = {
	.name = BMU_DRV_NAME,
	.probe = bmu_probe,
	.disconnect = bmu_disconnect,
	.id_table = bmu_table,
	.disable_hub_initiated_lpm = 1,
};

/* brcmsmac's per-device debugfs files live under brcmsmac_usb_wl/ (brcmsmac/debug.c) */
void brcms_debugfs_init(void);
void brcms_debugfs_exit(void);

static int __init bmu_module_init(void)
{
	int ret;

	brcms_debugfs_init();
	ret = usb_register(&bmu_driver);
	if (ret)
		brcms_debugfs_exit();
	return ret;
}

static void __exit bmu_module_exit(void)
{
	usb_deregister(&bmu_driver);
	brcms_debugfs_exit();
}

module_init(bmu_module_init);
module_exit(bmu_module_exit);

MODULE_AUTHOR("brcmsmac-usb contributors");
MODULE_DESCRIPTION("Broadcom BCM4322 USB BMAC (split-MAC) dongle driver");
MODULE_LICENSE("Dual BSD/GPL");
MODULE_FIRMWARE("brcm/bcm4322-bmac-5.100.148.5.trx");
