// SPDX-License-Identifier: ISC OR GPL-2.0
/*
 * Toolchain smoke test for brcmsmac-usb: proves out-of-tree modules build for and load on the
 * target kernel. Not part of the driver.
 */
#include <linux/init.h>
#include <linux/module.h>

static int __init bcm_hello_init(void)
{
	pr_info("brcmsmac-usb toolchain test: loaded\n");
	return 0;
}

static void __exit bcm_hello_exit(void)
{
	pr_info("brcmsmac-usb toolchain test: unloaded\n");
}

module_init(bcm_hello_init);
module_exit(bcm_hello_exit);

MODULE_DESCRIPTION("brcmsmac-usb toolchain smoke test");
MODULE_LICENSE("Dual BSD/GPL");
