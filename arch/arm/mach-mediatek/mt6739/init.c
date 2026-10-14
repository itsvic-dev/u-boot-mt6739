// SPDX-License-Identifier: GPL-2.0

#include <cpu_func.h>
#include <fdtdec.h>
#include <init.h>
#include <asm/global_data.h>
#include <asm/io.h>
#include <linux/sizes.h>

DECLARE_GLOBAL_DATA_PTR;

#define WDT_BASE		0x10007000
#define WDT_MODE		0x00
#define WDT_MODE_EN		BIT(0)
#define WDT_MODE_EXRST_EN	BIT(2)
#define WDT_MODE_IRQ_EN		BIT(3)
#define WDT_MODE_AUTO_START	BIT(4)
#define WDT_MODE_DUAL_EN	BIT(6)
#define WDT_MODE_DDR_RESERVE	BIT(7)
#define WDT_MODE_KEY		0x22000000
#define WDT_SWRST		0x14
#define WDT_SWRST_KEY		0x1209

/* The modem, CONSYS, framebuffer and ATF regions start at 0x61fff000. */
#define USABLE_RAM_TOP		0x60000000

int dram_init(void)
{
	int ret;

	ret = fdtdec_setup_memory_banksize();
	if (ret)
		return ret;

	return fdtdec_setup_mem_size_base();
}

phys_addr_t board_get_usable_ram_top(phys_size_t total_size)
{
	return min_t(phys_addr_t, gd->ram_top, USABLE_RAM_TOP);
}

/* PSCI SYSTEM_RESET hangs in the ATF; LK stalls on DDR_RESERVE, AUTO_START skips the power key. */
void reset_cpu(void)
{
	void __iomem *wdt = (void __iomem *)WDT_BASE;
	u32 mode = readl(wdt + WDT_MODE);

	mode &= ~(WDT_MODE_EN | WDT_MODE_IRQ_EN | WDT_MODE_DUAL_EN |
		  WDT_MODE_DDR_RESERVE);
	mode |= WDT_MODE_EXRST_EN | WDT_MODE_AUTO_START;
	writel(mode | WDT_MODE_KEY, wdt + WDT_MODE);
	writel(WDT_SWRST_KEY, wdt + WDT_SWRST);

	while (1)
		;
}

int print_cpuinfo(void)
{
	printf("CPU:   MediaTek MT6739\n");
	return 0;
}
