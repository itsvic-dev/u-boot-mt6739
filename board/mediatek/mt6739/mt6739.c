// SPDX-License-Identifier: GPL-2.0

#include <dm.h>
#include <env.h>
#include <fdt_support.h>
#include <init.h>
#include <malloc.h>
#include <usb.h>
#include <asm/io.h>
#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/libfdt.h>

#define TOPCKGEN		0x10000000
#define CLK_CFG_UPDATE		0x004
#define CLK_CFG_7_SET		0x0b4
#define CLK_CFG_7_CLR		0x0b8
#define USB_TOP_SEL_UNIVPLL3_D4	BIT(24)
#define USB_TOP_SEL_PDN		BIT(31)
#define USB_TOP_SEL_UPDATE	BIT(30)

#define INFRACFG_AO		0x10001000
#define INFRA_CG0_CLR		0x084
#define INFRA_CG0_ICUSB		BIT(8)

int board_init(void)
{
	return 0;
}

/* LK does not use USB in normal boot, so it may leave the USB clocks off. */
int board_usb_init(int index, enum usb_init_type init)
{
	struct udevice *dev;

	writel(USB_TOP_SEL_PDN, (void __iomem *)TOPCKGEN + CLK_CFG_7_CLR);
	writel(USB_TOP_SEL_UNIVPLL3_D4, (void __iomem *)TOPCKGEN + CLK_CFG_7_SET);
	writel(USB_TOP_SEL_UPDATE, (void __iomem *)TOPCKGEN + CLK_CFG_UPDATE);
	writel(INFRA_CG0_ICUSB, (void __iomem *)INFRACFG_AO + INFRA_CG0_CLR);

	return uclass_get_device(UCLASS_USB_GADGET_GENERIC, index, &dev);
}

/* LK puts its own arguments and the boot mode (third word of atag,boot) in its /chosen. */
int board_late_init(void)
{
	const void *fdt = (const void *)get_prev_bl_fdt_addr();
	const char *args;
	const u32 *tag;
	int node, len;

	if (!fdt || fdt_check_header(fdt))
		return 0;

	env_set_hex("fdt_addr", (ulong)fdt);

	node = fdt_path_offset(fdt, "/chosen");
	if (node < 0)
		return 0;

	args = fdt_getprop(fdt, node, "bootargs", NULL);
	if (args) {
		const char *sn = strstr(args, "androidboot.serialno=");

		env_set("lkargs", args);
		if (sn) {
			char serial[33] = "";

			sn += strlen("androidboot.serialno=");
			strncat(serial, sn, min(strcspn(sn, " "), sizeof(serial) - 1));
			env_set("serial#", serial);
		}
	}

	tag = fdt_getprop(fdt, node, "atag,boot", &len);
	if (tag && len >= 12)
		env_set_ulong("lk_bootmode", le32_to_cpu(tag[2]));

	return 0;
}

/* Linux gets the LK arguments first and then the ones of the OS, as LK itself does. */
int ft_board_setup(void *blob, struct bd_info *bd)
{
	const char *lkargs = env_get("lkargs");
	const char *args;
	char *all;
	int node, ret;

	if (!lkargs)
		return 0;

	node = fdt_find_or_add_subnode(blob, 0, "chosen");
	if (node < 0)
		return node;

	args = fdt_getprop(blob, node, "bootargs", NULL);
	all = malloc(strlen(lkargs) + (args ? strlen(args) : 0) + 2);
	if (!all)
		return -ENOMEM;

	strcpy(all, lkargs);
	if (args && *args) {
		strcat(all, " ");
		strcat(all, args);
	}
	ret = fdt_setprop_string(blob, node, "bootargs", all);
	free(all);

	return ret;
}
