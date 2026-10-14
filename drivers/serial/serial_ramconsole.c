// SPDX-License-Identifier: GPL-2.0
/* Output-only console in a Linux persistent_ram zone (earlycon=pstoreram format). */

#include <cpu_func.h>
#include <dm.h>
#include <serial.h>
#include <asm/cache.h>
#include <asm/io.h>
#include <linux/kernel.h>

#define RAMCONSOLE_SIG	0x43474244

struct ramconsole_zone {
	u32 sig;
	u32 start;
	u32 size;
	u8 data[];
};

struct ramconsole_priv {
	struct ramconsole_zone *zone;
	u32 cap;
};

static void ramconsole_flush(void *p, size_t len)
{
	ulong start = rounddown((ulong)p, ARCH_DMA_MINALIGN);
	ulong end = roundup((ulong)p + len, ARCH_DMA_MINALIGN);

	flush_dcache_range(start, end);
}

static void ramconsole_init(struct ramconsole_zone *z, u32 cap)
{
	if (z->sig != RAMCONSOLE_SIG || z->start >= cap || z->size > cap) {
		z->start = 0;
		z->size = 0;
		z->sig = RAMCONSOLE_SIG;
		ramconsole_flush(z, sizeof(*z));
	}
}

static void ramconsole_write(struct ramconsole_zone *z, u32 cap, const char *s, size_t len)
{
	u32 start = z->start;
	size_t i;

	for (i = 0; i < len; i++) {
		z->data[start] = s[i];
		ramconsole_flush(&z->data[start], 1);
		if (++start == cap)
			start = 0;
		if (z->size < cap)
			z->size++;
	}
	z->start = start;
	ramconsole_flush(z, sizeof(*z));
}

static ssize_t ramconsole_puts(struct udevice *dev, const char *s, size_t len)
{
	struct ramconsole_priv *priv = dev_get_priv(dev);

	ramconsole_write(priv->zone, priv->cap, s, len);
	return len;
}

static int ramconsole_putc(struct udevice *dev, const char ch)
{
	return ramconsole_puts(dev, &ch, 1) == 1 ? 0 : -EIO;
}

static int ramconsole_getc(struct udevice *dev)
{
	return -EAGAIN;
}

static int ramconsole_pending(struct udevice *dev, bool input)
{
	return 0;
}

static int ramconsole_probe(struct udevice *dev)
{
	struct ramconsole_priv *priv = dev_get_priv(dev);
	struct ramconsole_zone *z;
	fdt_size_t size;

	z = (struct ramconsole_zone *)dev_read_addr_size(dev, &size);
	if ((fdt_addr_t)z == FDT_ADDR_T_NONE || size <= sizeof(*z))
		return -EINVAL;

	priv->zone = z;
	priv->cap = size - sizeof(*z);
	ramconsole_init(z, priv->cap);

	return 0;
}

static const struct dm_serial_ops ramconsole_ops = {
	.putc = ramconsole_putc,
	.puts = ramconsole_puts,
	.getc = ramconsole_getc,
	.pending = ramconsole_pending,
};

static const struct udevice_id ramconsole_ids[] = {
	{ .compatible = "u-boot,ramconsole" },
	{ }
};

U_BOOT_DRIVER(serial_ramconsole) = {
	.name = "serial_ramconsole",
	.id = UCLASS_SERIAL,
	.of_match = ramconsole_ids,
	.probe = ramconsole_probe,
	.priv_auto = sizeof(struct ramconsole_priv),
	.ops = &ramconsole_ops,
	.flags = DM_FLAG_PRE_RELOC,
};

#ifdef CONFIG_DEBUG_UART_RAMCONSOLE
#include <debug_uart.h>

#define DEBUG_ZONE	((struct ramconsole_zone *)CONFIG_VAL(DEBUG_UART_BASE))
#define DEBUG_CAP	(CONFIG_DEBUG_UART_RAMCONSOLE_SIZE - sizeof(struct ramconsole_zone))

static inline void _debug_uart_init(void)
{
	ramconsole_init(DEBUG_ZONE, DEBUG_CAP);
}

static inline void _debug_uart_putc(int c)
{
	char ch = c;

	ramconsole_write(DEBUG_ZONE, DEBUG_CAP, &ch, 1);
}

DEBUG_UART_FUNCS
#endif
