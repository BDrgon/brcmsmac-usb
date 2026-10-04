// SPDX-License-Identifier: ISC
/*
 * brcmsmac_usb — debugfs: /sys/kernel/debug/brcmsmac_usb-<intf>/{state,stats,trace,rpc,fault}
 *
 *   rpc:   write "sync ID [ARG...]" or "call ID [ARG...]" (ARGs are u32 words, any base);
 *          read back the last status and reply bytes.
 *   fault: write "hang" to simulate a hung dongle (exercises the recovery path).
 */
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include "bmu.h"

static const char * const bmu_state_names[] = {
	"probing", "downloading", "connecting", "up", "hung", "gone",
};

static int bmu_state_show(struct seq_file *s, void *unused)
{
	struct bmu_dev *d = s->private;

	seq_printf(s, "state:      %s\n", bmu_state_names[READ_ONCE(d->state)]);
	seq_printf(s, "firmware:   %s (0x%08x) %s\n", d->fw->version, d->fw->verword, d->fw->file);
	seq_printf(s, "session:    0x%02x  seq %u%s\n", d->session, d->seq,
		   d->reconnected ? "  (reconnected)" : "");
	seq_printf(s, "bmac_init:  %s\n", d->bmac_init_sent ? "sent" : "not sent (register access refused)");
	seq_printf(s, "chip:       0x%04x\n",
		   d->revinfo_len >= 28 ? get_unaligned_le32(d->revinfo + 24) : 0);
	seq_printf(s, "mac:        %pM\n", d->macaddr);
	seq_printf(s, "fifos:      %u %u %u %u %u %u\n", d->fifo_sz[0], d->fifo_sz[1],
		   d->fifo_sz[2], d->fifo_sz[3], d->fifo_sz[4], d->fifo_sz[5]);
	seq_printf(s, "nvram:      %s\n", d->nvram_head);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(bmu_state);

static int bmu_stats_show(struct seq_file *s, void *unused)
{
	struct bmu_dev *d = s->private;
	struct bmu_stats *st = &d->stats;
	int i;

	seq_printf(s, "tx_msgs %llu\ntx_bytes %llu\ntx_errs %llu\n",
		   st->tx_msgs, st->tx_bytes, st->tx_errs);
	seq_printf(s, "rx_xfers %llu\nrx_msgs %llu\nrx_bytes %llu\nrx_errs %llu\nrx_bad_frames %llu\n",
		   st->rx_xfers, st->rx_msgs, st->rx_bytes, st->rx_errs, st->rx_bad_frames);
	seq_printf(s, "sync_calls %llu\nsync_timeouts %llu\nsync_errs %llu\nrecoveries %llu\n",
		   st->sync_calls, st->sync_timeouts, st->sync_errs, st->recoveries);
	for (i = 0; i < 256; i++)
		if (st->async_by_id[i])
			seq_printf(s, "async_id_%u %llu\n", i, st->async_by_id[i]);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(bmu_stats);

static int bmu_trace_show(struct seq_file *s, void *unused)
{
	struct bmu_dev *d = s->private;
	struct bmu_trace_ent *ents;
	unsigned long flags;
	unsigned int head, i, n;

	ents = kvmalloc_array(BMU_TRACE_LEN, sizeof(*ents), GFP_KERNEL);
	if (!ents)
		return -ENOMEM;
	spin_lock_irqsave(&d->trace_lock, flags);
	memcpy(ents, d->trace, sizeof(d->trace));
	head = d->trace_head;
	spin_unlock_irqrestore(&d->trace_lock, flags);

	n = min_t(unsigned int, head, BMU_TRACE_LEN);
	for (i = head - n; i != head; i++) {
		struct bmu_trace_ent *e = &ents[i % BMU_TRACE_LEN];

		u32 j, m = min_t(u32, e->len, BMU_TRACE_BYTES);

		seq_printf(s, "%lld %c k%u s%04x id%-3u len %-4u ",
			   ktime_to_us(e->t), e->dir, e->kind, e->seq, e->id, e->len);
		for (j = 0; j < m; j += 64)	/* %*ph prints at most 64 bytes */
			seq_printf(s, "%*phN", (int)min_t(u32, 64, m - j), e->bytes + j);
		seq_putc(s, '\n');
	}
	kvfree(ents);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(bmu_trace);

/* rpc: "sync ID [ARG...]" / "call ID [ARG...]" */
static ssize_t bmu_rpc_write(struct file *f, const char __user *ubuf, size_t count, loff_t *ppos)
{
	struct bmu_dev *d = ((struct seq_file *)f->private_data)->private;
	char *buf, *p, *tok;
	__le32 args[64];
	u32 id, v, n = 0, rlen = 0;
	bool sync;
	int ret;

	if (count > 1024)
		return -E2BIG;
	buf = memdup_user_nul(ubuf, count);
	if (IS_ERR(buf))
		return PTR_ERR(buf);
	p = strim(buf);
	tok = strsep(&p, " \t");
	if (!tok || (strcmp(tok, "sync") && strcmp(tok, "call"))) {
		ret = -EINVAL;
		goto out;
	}
	sync = !strcmp(tok, "sync");
	tok = strsep(&p, " \t");
	if (!tok || kstrtou32(tok, 0, &id)) {
		ret = -EINVAL;
		goto out;
	}
	while ((tok = strsep(&p, " \t"))) {
		if (!*tok)
			continue;
		if (n >= ARRAY_SIZE(args) || kstrtou32(tok, 0, &v)) {
			ret = -EINVAL;
			goto out;
		}
		args[n++] = cpu_to_le32(v);
	}
	if (sync)
		ret = bmu_sync(d, id, args, n * 4, d->raw_reply, sizeof(d->raw_reply), &rlen);
	else
		ret = bmu_call(d, id, args, n * 4, GFP_KERNEL);
	d->raw_status = ret;
	d->raw_reply_len = ret ? 0 : min_t(u32, rlen, sizeof(d->raw_reply));
	ret = ret ? ret : count;
out:
	kfree(buf);
	return ret;
}

static int bmu_rpc_show(struct seq_file *s, void *unused)
{
	struct bmu_dev *d = s->private;
	u32 i;

	seq_printf(s, "status %d len %u\n", d->raw_status, d->raw_reply_len);
	for (i = 0; i < d->raw_reply_len; i += 32)
		seq_printf(s, "%04x: %*phN\n", i, (int)min_t(u32, 32, d->raw_reply_len - i),
			   d->raw_reply + i);
	return 0;
}

static int bmu_rpc_open(struct inode *inode, struct file *f)
{
	return single_open(f, bmu_rpc_show, inode->i_private);
}

static const struct file_operations bmu_rpc_fops = {
	.owner = THIS_MODULE,
	.open = bmu_rpc_open,
	.read = seq_read,
	.write = bmu_rpc_write,
	.llseek = seq_lseek,
	.release = single_release,
};

static ssize_t bmu_fault_write(struct file *f, const char __user *ubuf, size_t count,
			       loff_t *ppos)
{
	struct bmu_dev *d = file_inode(f)->i_private;
	char buf[16];

	if (count >= sizeof(buf) || copy_from_user(buf, ubuf, count))
		return -EINVAL;
	buf[count] = 0;
	if (sysfs_streq(buf, "hang"))
		bmu_mark_hung(d, "fault injected via debugfs");
	else
		return -EINVAL;
	return count;
}

static const struct file_operations bmu_fault_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = bmu_fault_write,
};

void bmu_debugfs_init(struct bmu_dev *d)
{
	char name[64];

	snprintf(name, sizeof(name), "%s-%s", BMU_DRV_NAME, dev_name(&d->intf->dev));
	d->dbg_dir = debugfs_create_dir(name, NULL);
	if (IS_ERR(d->dbg_dir))
		return;
	debugfs_create_file("state", 0444, d->dbg_dir, d, &bmu_state_fops);
	debugfs_create_file("stats", 0444, d->dbg_dir, d, &bmu_stats_fops);
	debugfs_create_file("trace", 0444, d->dbg_dir, d, &bmu_trace_fops);
	debugfs_create_file("rpc", 0600, d->dbg_dir, d, &bmu_rpc_fops);
	debugfs_create_file("fault", 0200, d->dbg_dir, d, &bmu_fault_fops);
}

void bmu_debugfs_exit(struct bmu_dev *d)
{
	debugfs_remove_recursive(d->dbg_dir);
	d->dbg_dir = NULL;
}
