// SPDX-License-Identifier: GPL-2.0

#include <env.h>
#include <fdt_support.h>
#include <init.h>
#include <malloc.h>
#include <linux/errno.h>
#include <linux/libfdt.h>

int board_init(void)
{
	return 0;
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
	if (args)
		env_set("lkargs", args);

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
