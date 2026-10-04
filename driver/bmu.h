/* SPDX-License-Identifier: ISC */
/*
 * brcmsmac_usb — Broadcom BCM4322 USB "BMAC" (split-MAC) transport and RPC core.
 *
 * Wire protocol: docs/protocol.md. The dongle runs the LOW MAC (d11 core, PHY, radio) from
 * firmware downloaded at plug-in; the host drives it through RPC messages:
 *
 *   transport frame:  __le32 tp_len | message           (tp_len excludes itself)
 *   message:          __le32 hdr = kind << 24 | session << 16 | seq,  payload
 *   CALL/SYNC payload: __le32 rpc_id, arguments (LE u32 words, raw bytes padded to 4)
 *
 * Host→dongle messages go on bulk OUT; one-way dongle→host messages arrive on bulk IN
 * (possibly several per transfer); the reply to a SYNC call is read with vendor control
 * IN request 9.
 */
#ifndef BMU_H
#define BMU_H

#include <linux/types.h>
#include <linux/usb.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <linux/completion.h>
#include <linux/unaligned.h>

#define BMU_DRV_NAME		"brcmsmac_usb"

/* USB */
#define BMU_EP_OUT		0x03
#define BMU_EP_IN		0x82
#define BMU_RX_URBS		8
#define BMU_RX_BUFSZ		8192
#define BMU_MSG_MAX		2048	/* largest message we build (TX frame + headers) */

/* Vendor requests (bmRequestType 0xC1 IN / 0x41 OUT, recipient interface) */
#define BMU_DL_GETSTATE		0
#define BMU_DL_GO		2
#define BMU_DL_START		3
#define BMU_DL_REBOOT		4
#define BMU_DL_GETVER		5
#define BMU_DL_RESETCFG		8
#define BMU_RPC_RETURN		9
#define BMU_RETURN_WLEN		1928
#define BMU_RDL_CHUNK		1500

#define BMU_RDL_WAITING		0
#define BMU_RDL_RUNNABLE	4

#define BMU_CHIP_ROM		0x4322
#define BMU_CHIP_RUNNING	0xa123

/* Message kinds (our labels; docs/protocol.md §4.1) */
#define BMU_KIND_CALL		1
#define BMU_KIND_SYNC		2
#define BMU_KIND_MGMT		3

#define BMU_MGMT_HELLO		1
#define BMU_MGMT_CONNECT	2
#define BMU_MGMT_ACCEPT		3
#define BMU_MGMT_REFUSE		5

/* rpc ids used by the transport core itself (firmware 5.100.148.5 numbering) */
#define BMU_ID_REG_READ		1
#define BMU_ID_REG_WRITE	2
#define BMU_ID_BMAC_INIT	18
#define BMU_ID_COPYFROM_OBJMEM	39
#define BMU_ID_COPYTO_OBJMEM	40
#define BMU_ID_READ_SHM		44
#define BMU_ID_READ_TSF		45
#define BMU_ID_SET_RCMTA	48
#define BMU_ID_SET_SHM		49
#define BMU_ID_WRITE_SHM	51
#define BMU_ID_WRITE_TEMPLATE	52
#define BMU_ID_HW_ETHERADDR	56
#define BMU_ID_REVINFO_GET	75
#define BMU_ID_STATE_GET	76
#define BMU_ID_XMTFIFO_SZ_GET	77
#define BMU_ID_COPYFROM_VARS	91

#define BMU_SYNC_TIMEOUT_MS	2000
#define BMU_MGMT_TIMEOUT_MS	1000
#define BMU_TRACE_LEN		2048
#define BMU_TRACE_BYTES		256

struct bmu_fw {
	const char *version;	/* "5.100.148.5" */
	u32 verword;		/* CONNECT version word */
	const char *file;	/* request_firmware() name */
};

enum bmu_state {
	BMU_ST_PROBING,
	BMU_ST_DOWNLOADING,
	BMU_ST_CONNECTING,
	BMU_ST_UP,		/* session established, calls allowed */
	BMU_ST_HUNG,		/* stopped answering; needs reset or replug */
	BMU_ST_GONE,		/* disconnect in progress */
};

struct bmu_trace_ent {
	ktime_t t;
	u8 dir;			/* 'O' host→dongle, 'I' bulk IN, 'R' sync return */
	u8 kind;
	u16 seq;
	u32 id;
	u32 len;
	u8 bytes[BMU_TRACE_BYTES];
};

struct bmu_stats {
	u64 tx_msgs, tx_bytes, tx_errs;
	u64 rx_xfers, rx_msgs, rx_bytes, rx_errs, rx_bad_frames;
	u64 sync_calls, sync_timeouts, sync_errs;
	u64 async_by_id[256];
	u64 recoveries;
};

struct bmu_dev;
struct ieee80211_hw;

/* Handler for one-way dongle→host calls; runs in URB completion (atomic) context. */
typedef void (*bmu_async_fn)(void *ctx, u32 id, const u8 *args, u32 len);

struct bmu_dev {
	struct usb_device *udev;
	struct usb_interface *intf;
	unsigned int pipe_out, pipe_in;

	enum bmu_state state;
	spinlock_t lock;		/* state, seq, tx submission order, async handler */
	spinlock_t trace_lock;
	struct mutex sync_mutex;	/* one SYNC call (and its reply read) at a time */
	struct work_struct init_work;
	struct delayed_work recover_work;

	const struct bmu_fw *fw;
	u8 session;
	u16 seq;
	bool reconnected;
	bool bmac_init_sent;		/* register access allowed only after BMAC_INIT */

	/* MGMT replies arrive on bulk IN */
	struct completion mgmt_done;
	u8 mgmt_reply[32];
	u32 mgmt_len;

	/* async RX */
	struct usb_anchor rx_anchor;
	struct usb_anchor tx_anchor;
	struct urb *rx_urb[BMU_RX_URBS];
	bmu_async_fn async_fn;		/* called under lock; must not sleep or submit */
	void *async_ctx;

	struct ieee80211_hw *wl;	/* brcmsmac HIGH MAC (brcmsmac/mac80211_if.c), if attached */

	/* identity (filled after connect) */
	u8 revinfo[124];
	u32 revinfo_len;
	u8 macaddr[6];
	char nvram_head[64];
	u32 fifo_sz[6];

	/* control transfer bounce buffers (DMA-safe) */
	u8 *ctrl_buf;			/* BMU_RETURN_WLEN */

	struct bmu_stats stats;
	struct bmu_trace_ent trace[BMU_TRACE_LEN];
	unsigned int trace_head;

	struct dentry *dbg_dir;
	u8 raw_reply[BMU_RETURN_WLEN];
	u32 raw_reply_len;
	int raw_status;
};

/* bmu_usb.c */
int bmu_vreq_in(struct bmu_dev *d, u8 req, void *buf, u16 len, int timeout_ms);
int bmu_getver(struct bmu_dev *d, u32 *chip);
int bmu_download(struct bmu_dev *d, const u8 *img, size_t len);
int bmu_rx_start(struct bmu_dev *d);
void bmu_rx_stop(struct bmu_dev *d);
int bmu_submit_msg(struct bmu_dev *d, u8 kind, const void *payload, u32 len, gfp_t gfp,
		   struct completion *done, int *status);

/* bmu_rpc.c */
int bmu_connect(struct bmu_dev *d);
int bmu_call(struct bmu_dev *d, u32 id, const void *args, u32 arglen, gfp_t gfp);
int bmu_sync(struct bmu_dev *d, u32 id, const void *args, u32 arglen,
	     void *ret, u32 retmax, u32 *retlen);
void bmu_rx_frames(struct bmu_dev *d, const u8 *buf, u32 len);
void bmu_set_async_handler(struct bmu_dev *d, bmu_async_fn fn, void *ctx);
void bmu_mark_hung(struct bmu_dev *d, const char *why);
int bmu_identify(struct bmu_dev *d);

static inline u32 bmu_sync_u32(const u8 *p) { return get_unaligned_le32(p); }

/* trace */
void bmu_trace(struct bmu_dev *d, u8 dir, u8 kind, u16 seq, u32 id, const void *data, u32 len);

/* bmu_debugfs.c */
void bmu_debugfs_init(struct bmu_dev *d);
void bmu_debugfs_exit(struct bmu_dev *d);

extern bool bmu_param_allow_reset;

#endif /* BMU_H */
