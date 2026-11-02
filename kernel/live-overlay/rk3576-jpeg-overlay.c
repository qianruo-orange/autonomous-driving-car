// SPDX-License-Identifier: GPL-2.0
/* Activate JPEG nodes on a running kernel before the next boot uses the new DTB. */
#include <linux/module.h>
#include <linux/firmware.h>
#include <linux/of.h>
static int overlay_id;
static int __init jpeg_overlay_init(void)
{
 const struct firmware *blob;
 int rc = request_firmware_direct(&blob, "autonomous-driving-car/rk3576-jpeg.dtbo", NULL);
 if (rc) return rc;
 rc = of_overlay_fdt_apply(blob->data, blob->size, &overlay_id, NULL);
 release_firmware(blob);
 return rc;
}
static void __exit jpeg_overlay_exit(void) { of_overlay_remove(&overlay_id); }
module_init(jpeg_overlay_init);
module_exit(jpeg_overlay_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("RK3576 JPEG device-tree activation");
