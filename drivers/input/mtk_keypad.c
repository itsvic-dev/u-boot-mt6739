// SPDX-License-Identifier: GPL-2.0
/* MediaTek keypad (MT6739, MT6779), polled. */

#include <dm.h>
#include <input.h>
#include <keyboard.h>
#include <malloc.h>
#include <stdio_dev.h>
#include <asm/io.h>
#include <linux/bitops.h>

#define KPD_MEM			0x0004
#define KPD_NUM_MEMS		5
#define KPD_BITS_PER_MEM	16

struct mtk_keypad_priv {
	void __iomem *base;
	u32 cols;
	u16 state[KPD_NUM_MEMS];
	u16 *keymap;
	u32 keymap_size;
};

static int mtk_keypad_read_keys(struct input_config *input)
{
	struct mtk_keypad_priv *priv = dev_get_priv(input->dev);
	int i, bit;

	for (i = 0; i < KPD_NUM_MEMS; i++) {
		u16 now = readl(priv->base + KPD_MEM + 4 * i);
		u16 changed = now ^ priv->state[i];

		priv->state[i] = now;
		for (bit = 0; bit < KPD_BITS_PER_MEM; bit++) {
			u32 key = i * KPD_BITS_PER_MEM + bit;

			if (!(changed & BIT(bit)) || key >= priv->keymap_size ||
			    !priv->keymap[key])
				continue;
			/* A key is down when its bit is 0. */
			input_add_keycode(input, priv->keymap[key], !(now & BIT(bit)));
		}
	}

	return 0;
}

static int mtk_keypad_of_to_plat(struct udevice *dev)
{
	struct mtk_keypad_priv *priv = dev_get_priv(dev);
	const u32 *map;
	u32 rows;
	int len, i;

	priv->base = dev_read_addr_ptr(dev);
	if (!priv->base)
		return -EINVAL;

	if (dev_read_u32(dev, "keypad,num-rows", &rows) ||
	    dev_read_u32(dev, "keypad,num-columns", &priv->cols) || !priv->cols)
		return -EINVAL;

	priv->keymap_size = rows * priv->cols;
	priv->keymap = calloc(priv->keymap_size, sizeof(*priv->keymap));
	if (!priv->keymap)
		return -ENOMEM;

	map = dev_read_prop(dev, "linux,keymap", &len);
	if (!map)
		return -EINVAL;

	for (i = 0; i < len / 4; i++) {
		u32 entry = fdt32_to_cpu(map[i]);
		u32 row = entry >> 24, col = (entry >> 16) & 0xff;

		if (row < rows && col < priv->cols)
			priv->keymap[row * priv->cols + col] = entry & 0xffff;
	}

	return 0;
}

static int mtk_keypad_probe(struct udevice *dev)
{
	struct mtk_keypad_priv *priv = dev_get_priv(dev);
	struct keyboard_priv *uc_priv = dev_get_uclass_priv(dev);
	struct stdio_dev *sdev = &uc_priv->sdev;
	struct input_config *input = &uc_priv->input;
	int i;

	for (i = 0; i < KPD_NUM_MEMS; i++)
		priv->state[i] = readl(priv->base + KPD_MEM + 4 * i);

	input_init(input, false);
	input_add_tables(input, false);
	input->dev = dev;
	input->read_keys = mtk_keypad_read_keys;
	strcpy(sdev->name, "keypad");

	return input_stdio_register(sdev);
}

static const struct udevice_id mtk_keypad_ids[] = {
	{ .compatible = "mediatek,mt6779-keypad" },
	{ }
};

U_BOOT_DRIVER(mtk_keypad) = {
	.name = "mtk_keypad",
	.id = UCLASS_KEYBOARD,
	.of_match = mtk_keypad_ids,
	.of_to_plat = mtk_keypad_of_to_plat,
	.probe = mtk_keypad_probe,
	.priv_auto = sizeof(struct mtk_keypad_priv),
};
