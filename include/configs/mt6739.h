/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __MT6739_H
#define __MT6739_H

#define CFG_EXTRA_ENV_SETTINGS \
	"stdin=serial,keypad\0" \
	"stdout=serial,vidconsole\0" \
	"stderr=serial,vidconsole\0" \
	"boot_nixos=fdt addr ${prevbl_fdt_addr}; " \
		"fdt get value bootargs /chosen bootargs; " \
		"part start mmc 0 boot boot_start; " \
		"part size mmc 0 boot boot_size; " \
		"mmc read 0x48000000 ${boot_start} ${boot_size}; " \
		"bootm 0x48000000 0x48000000 ${prevbl_fdt_addr}\0" \
	"bootmenu_0=NixOS=run boot_nixos\0" \
	"bootmenu_1=Reboot=reset\0"

#endif
