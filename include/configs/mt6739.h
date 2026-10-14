/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __MT6739_H
#define __MT6739_H

#define CFG_EXTRA_ENV_SETTINGS \
	"stdin=serial,keypad\0" \
	"stdout=serial,vidconsole\0" \
	"stderr=serial,vidconsole\0" \
	"kernel_addr_r=0x40200000\0" \
	"ramdisk_addr_r=0x48000000\0" \
	"pxefile_addr_r=0x4f000000\0" \
	"scriptaddr=0x4f100000\0" \
	"android_addr_r=0x50000000\0" \
	"find_bootpart=part number mmc 0 vendor bootpart\0" \
	"boot_nixos=run find_bootpart; " \
		"sysboot mmc 0:${bootpart} fat ${pxefile_addr_r} /extlinux/extlinux.conf\0" \
	"boot_nixos_menu=run find_bootpart; " \
		"sysboot -p mmc 0:${bootpart} fat ${pxefile_addr_r} /extlinux/extlinux.conf\0" \
	"boot_bootimg=setenv bootargs; " \
		"part start mmc 0 boot boot_start; " \
		"part size mmc 0 boot boot_size; " \
		"mmc read ${android_addr_r} ${boot_start} ${boot_size}; " \
		"bootm ${android_addr_r} ${android_addr_r} ${fdt_addr}\0" \
	"boot_lineage=run find_bootpart; setenv bootargs; " \
		"fatload mmc 0:${bootpart} ${android_addr_r} /android/boot.img && " \
		"fdt addr ${fdt_addr} && fdt resize && " \
		"fdt set /firmware/android compatible android,firmware && " \
		"fdt set /firmware/android boot_devices soc/11240000.mmc && " \
		"bootm ${android_addr_r} ${android_addr_r} ${fdt_addr}\0" \
	"bootmenu_0=NixOS=run boot_nixos\0" \
	"bootmenu_1=NixOS generations=run boot_nixos_menu\0" \
	"bootmenu_2=LineageOS=run boot_lineage\0" \
	"bootmenu_3=NixOS (boot.img)=run boot_bootimg\0" \
	"bootmenu_4=Reboot=reset\0"

#endif
