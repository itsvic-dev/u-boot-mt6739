// SPDX-License-Identifier: GPL-2.0
/* MT6739 DBI command-mode display path, as left on by the vendor LK. */

#include <cpu_func.h>
#include <dm.h>
#include <log.h>
#include <video.h>
#include <asm/cache.h>
#include <asm/io.h>
#include <linux/bitfield.h>
#include <linux/bitops.h>

#define OVL_SRC_CON		0x02c
#define OVL_SRC_CON_L3_EN	BIT(3)
#define OVL_L3_CON		0x090
#define OVL_L3_PITCH		0x0a4
#define OVL_L3_ADDR		0xfa0
#define OVL_CON_CLRFMT		GENMASK(15, 12)
#define OVL_CON_CLRFMT_ARGB8888	2
#define OVL_CON_AEN		BIT(8)
#define OVL_CON_BYTE_SWAP	BIT(24)

#define MUTEX0_EN		0x020

#define DBI_STA			0x000
#define DBI_STA_BUSY		BIT(4)
#define DBI_INT_STATUS		0x008
#define DBI_START		0x00c
#define DBI_START_START		BIT(15)
#define DBI_ROI_SIZE		0x06c
#define DBI_ROI_WIDTH		GENMASK(10, 0)
#define DBI_ROI_HEIGHT		GENMASK(26, 16)
#define DBI_SCMD0		0xf80

#define MIPI_DCS_WRITE_MEMORY_START	0x2c

struct mt6739_dbi_priv {
	void __iomem *ovl;
	void __iomem *mutex;
	void __iomem *dbi;
};

static int mt6739_dbi_read_size(struct udevice *dev, u32 *width, u32 *height)
{
	void __iomem *dbi = dev_read_addr_name_ptr(dev, "dbi");
	u32 size;

	if (!dbi)
		return -EINVAL;

	size = readl(dbi + DBI_ROI_SIZE);
	*width = FIELD_GET(DBI_ROI_WIDTH, size);
	*height = FIELD_GET(DBI_ROI_HEIGHT, size);
	if (!*width || !*height)
		return -ENODEV;

	return 0;
}

/* The panel has its own frame memory; it shows the framebuffer only after a DBI transfer. */
static int mt6739_dbi_sync(struct udevice *dev)
{
	struct mt6739_dbi_priv *priv = dev_get_priv(dev);
	struct video_priv *uc_priv = dev_get_uclass_priv(dev);
	ulong fb = (ulong)uc_priv->fb;

	if (readl(priv->dbi + DBI_STA) & DBI_STA_BUSY)
		return 0;

	flush_dcache_range(rounddown(fb, ARCH_DMA_MINALIGN),
			   roundup(fb + uc_priv->fb_size, ARCH_DMA_MINALIGN));

	writel(0, priv->dbi + DBI_INT_STATUS);
	writel(MIPI_DCS_WRITE_MEMORY_START, priv->dbi + DBI_SCMD0);
	writel(0, priv->dbi + DBI_START);
	writel(DBI_START_START, priv->dbi + DBI_START);
	writel(1, priv->mutex + MUTEX0_EN);

	return 0;
}

static int mt6739_dbi_probe(struct udevice *dev)
{
	struct mt6739_dbi_priv *priv = dev_get_priv(dev);
	struct video_uc_plat *plat = dev_get_uclass_plat(dev);
	struct video_priv *uc_priv = dev_get_uclass_priv(dev);
	u32 width, height, con;
	int ret;

	priv->ovl = dev_read_addr_name_ptr(dev, "ovl");
	priv->mutex = dev_read_addr_name_ptr(dev, "mutex");
	priv->dbi = dev_read_addr_name_ptr(dev, "dbi");
	if (!priv->ovl || !priv->mutex || !priv->dbi)
		return -EINVAL;

	ret = mt6739_dbi_read_size(dev, &width, &height);
	if (ret)
		return log_msg_ret("size", ret);

	uc_priv->xsize = width;
	uc_priv->ysize = height;
	uc_priv->bpix = VIDEO_BPP32;
	uc_priv->format = VIDEO_X8R8G8B8;

	con = readl(priv->ovl + OVL_L3_CON);
	con &= ~(OVL_CON_CLRFMT | OVL_CON_AEN | OVL_CON_BYTE_SWAP);
	con |= FIELD_PREP(OVL_CON_CLRFMT, OVL_CON_CLRFMT_ARGB8888);
	writel(con, priv->ovl + OVL_L3_CON);
	writel(OVL_SRC_CON_L3_EN, priv->ovl + OVL_SRC_CON);
	writel(width * VNBYTES(VIDEO_BPP32), priv->ovl + OVL_L3_PITCH);
	writel(plat->base, priv->ovl + OVL_L3_ADDR);

	video_set_flush_dcache(dev, true);

	return 0;
}

static int mt6739_dbi_bind(struct udevice *dev)
{
	struct video_uc_plat *plat = dev_get_uclass_plat(dev);
	u32 width, height;
	int ret;

	ret = mt6739_dbi_read_size(dev, &width, &height);
	if (ret)
		return ret;

	plat->size = width * height * VNBYTES(VIDEO_BPP32);

	return 0;
}

static const struct video_ops mt6739_dbi_ops = {
	.video_sync = mt6739_dbi_sync,
};

static const struct udevice_id mt6739_dbi_ids[] = {
	{ .compatible = "mediatek,mt6739-dbi-display" },
	{ }
};

U_BOOT_DRIVER(mt6739_dbi) = {
	.name = "mt6739_dbi",
	.id = UCLASS_VIDEO,
	.of_match = mt6739_dbi_ids,
	.ops = &mt6739_dbi_ops,
	.bind = mt6739_dbi_bind,
	.probe = mt6739_dbi_probe,
	.priv_auto = sizeof(struct mt6739_dbi_priv),
};
