// SPDX-License-Identifier: GPL-2.0
/* Boot menu, NixOS generations and fastboot status for the AGM M7 screen. */

#include <command.h>
#include <console.h>
#include <cyclic.h>
#include <dm.h>
#include <env.h>
#include <fastboot.h>
#include <fs.h>
#include <g_dnl.h>
#include <malloc.h>
#include <mapmem.h>
#include <stdio.h>
#include <time.h>
#include <video.h>
#include <video_font.h>
#include <asm/io.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/usb/gadget.h>
#include <watchdog.h>

#define COL_BG		0x000000
#define COL_FG		0xe8e8e8
#define COL_DIM		0x8a8a8a
#define COL_ACCENT	0x7ebae4
#define COL_SEL_FG	0x0b1620
#define COL_LINE	0x2a3a48
#define COL_TRACK	0x1b2f42
#define COL_OK		0x7fd37f
#define COL_FAIL	0xe06c6c

#define DBI_STA		0x14013000
#define DBI_STA_BUSY	BIT(4)

#define COUNTDOWN_MS	3000
#define MAX_GENS	16
#define GEN_ROWS	8

enum key { K_NONE, K_UP, K_DOWN, K_LEFT, K_RIGHT, K_OK, K_BACK, K_OTHER };

struct gen {
	char label[32];
	char text[24];
	char version[40];
	bool current;
};

static struct udevice *vid;
static struct video_fontdata *font_small, *font_big;
static struct gen gens[MAX_GENS];
static int num_gens;
static int current_gen = -1;

static void ui_fill(int x, int y, int w, int h, u32 col)
{
	struct video_priv *priv = dev_get_uclass_priv(vid);
	int i, j;

	for (j = max(y, 0); j < min(y + h, (int)priv->ysize); j++) {
		u32 *line = priv->fb + j * priv->line_length;

		for (i = max(x, 0); i < min(x + w, (int)priv->xsize); i++)
			line[i] = col;
	}
}

static int ui_glyph(int x, int y, unsigned char c, struct video_fontdata *f, u32 col)
{
	struct video_priv *priv = dev_get_uclass_priv(vid);
	const u8 *bits = f->video_fontdata + c * f->char_pixel_bytes;
	int row, i;

	for (row = 0; row < f->height; row++, bits += f->byte_width) {
		u32 *line;

		if (y + row < 0 || y + row >= priv->ysize)
			continue;
		line = priv->fb + (y + row) * priv->line_length;
		for (i = 0; i < f->width; i++)
			if (bits[i / 8] & (0x80 >> (i % 8)) && x + i < priv->xsize)
				line[x + i] = col;
	}

	return f->width;
}

static void ui_text(int x, int y, const char *s, struct video_fontdata *f, u32 col, bool bold)
{
	for (; *s; s++) {
		ui_glyph(x, y, *s, f, col);
		if (bold)
			ui_glyph(x + 1, y, *s, f, col);
		x += f->width;
	}
}

static int ui_text_width(const char *s, struct video_fontdata *f)
{
	return strlen(s) * f->width;
}

static void ui_text_center(int y, const char *s, struct video_fontdata *f, u32 col)
{
	struct video_priv *priv = dev_get_uclass_priv(vid);

	ui_text((priv->xsize - ui_text_width(s, f)) / 2, y, s, f, col, f == font_big);
}

static void ui_text_right(int right, int y, const char *s, u32 col)
{
	ui_text(right - ui_text_width(s, font_small), y, s, font_small, col, false);
}

static void ui_clear(void)
{
	struct video_priv *priv = dev_get_uclass_priv(vid);

	ui_fill(0, 0, priv->xsize, priv->ysize, COL_BG);
}

static void ui_header(const char *title, const char *right)
{
	struct video_priv *priv = dev_get_uclass_priv(vid);

	ui_text(8, 6, title, font_small, COL_ACCENT, true);
	if (right)
		ui_text_right(priv->xsize - 8, 6, right, COL_DIM);
	ui_fill(0, 26, priv->xsize, 1, COL_LINE);
}

static void ui_footer(const char *s)
{
	ui_text(8, 298, s, font_small, COL_DIM, false);
}

static void ui_sel_row(int y, bool selected)
{
	struct video_priv *priv = dev_get_uclass_priv(vid);

	if (selected)
		ui_fill(0, y - 4, priv->xsize, 24, COL_ACCENT);
}

/* The panel only shows a frame after a whole DBI transfer, and the driver skips a sync while one runs. */
static void ui_flush(void)
{
	ulong start = get_timer(0);

	while ((readl((void __iomem *)DBI_STA) & DBI_STA_BUSY) && get_timer(start) < 100)
		udelay(100);
	video_sync(vid, true);
}

static enum key read_key(void)
{
	ulong start;
	int c;

	if (!tstc())
		return K_NONE;

	c = getchar();
	if (c == '\r' || c == '\n')
		return K_OK;
	if (c != 0x1b)
		return K_OTHER;

	start = get_timer(0);
	while (!tstc())
		if (get_timer(start) > 20)
			return K_BACK;
	if (getchar() != '[')
		return K_OTHER;
	while (!tstc())
		if (get_timer(start) > 20)
			return K_OTHER;
	switch (getchar()) {
	case 'A':
		return K_UP;
	case 'B':
		return K_DOWN;
	case 'C':
		return K_RIGHT;
	case 'D':
		return K_LEFT;
	}

	return K_OTHER;
}

static enum key wait_key(void)
{
	enum key k;

	while ((k = read_key()) == K_NONE)
		schedule();

	return k;
}

/* NixOS generations from extlinux.conf */

static char *field(char *line, const char *key)
{
	size_t n = strlen(key);

	while (*line == ' ' || *line == '\t')
		line++;
	if (strncasecmp(line, key, n) || (line[n] != ' ' && line[n] != '\t'))
		return NULL;
	line += n;
	while (*line == ' ' || *line == '\t')
		line++;

	return line;
}

static void copy_word(char *dst, size_t len, const char *src)
{
	size_t n = strcspn(src, " \t");

	snprintf(dst, len, "%.*s", (int)n, src);
}

/* "13: 2026-10-14 16:57" from nixos/bootloader.nix becomes "13  Oct 14 16:57". */
static void gen_text(struct gen *g, const char *menu)
{
	static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
	const char *colon = strchr(menu, ':');
	int month;

	if (!colon || strlen(colon) < 18 || colon[6] != '-' || colon[9] != '-') {
		snprintf(g->text, sizeof(g->text), "%s", menu);
		return;
	}
	month = (colon[7] - '0') * 10 + (colon[8] - '0');
	if (month < 1 || month > 12) {
		snprintf(g->text, sizeof(g->text), "%s", menu);
		return;
	}
	snprintf(g->text, sizeof(g->text), "%2.*s  %.3s %.2s %.5s",
		 (int)(colon - menu), menu, months + (month - 1) * 3, colon + 10, colon + 13);
}

static void gen_version(struct gen *g, const char *init)
{
	const char *end = strstr(init, "/init");
	const char *p;

	g->version[0] = '\0';
	if (!end)
		return;
	for (p = end; p > init && p[-1] != '-'; p--)
		;
	if (p > init)
		snprintf(g->version, sizeof(g->version), "%.*s", (int)(end - p), p);
}

static void load_generations(void)
{
	char def[32] = "", def_init[160] = "", init[MAX_GENS][160];
	char *buf, *line, *next, cur_init[160] = "";
	struct gen *g = NULL;
	bool is_default = false;
	loff_t len;
	int i;

	num_gens = 0;
	current_gen = -1;
	if (run_command("run find_bootpart", 0))
		return;
	buf = malloc(64 * 1024);
	if (!buf)
		return;
	{
		char dev[16];

		snprintf(dev, sizeof(dev), "0:%s", env_get("bootpart") ?: "1");
		if (fs_set_blk_dev("mmc", dev, FS_TYPE_ANY) ||
		    fs_read("/extlinux/extlinux.conf", map_to_sysmem(buf), 0, 64 * 1024 - 1, &len)) {
			free(buf);
			return;
		}
	}
	buf[len] = '\0';

	for (line = buf; line; line = next) {
		char *v;

		next = strchr(line, '\n');
		if (next)
			*next++ = '\0';

		if ((v = field(line, "DEFAULT"))) {
			copy_word(def, sizeof(def), v);
		} else if ((v = field(line, "LABEL"))) {
			char name[32];

			copy_word(name, sizeof(name), v);
			if (g && !is_default)
				snprintf(init[num_gens++], sizeof(init[0]), "%s", cur_init);
			else if (is_default)
				snprintf(def_init, sizeof(def_init), "%s", cur_init);
			cur_init[0] = '\0';
			is_default = def[0] && !strcmp(name, def);
			g = NULL;
			if (!is_default && num_gens < MAX_GENS) {
				g = &gens[num_gens];
				memset(g, 0, sizeof(*g));
				snprintf(g->label, sizeof(g->label), "%s", name);
			}
		} else if ((v = field(line, "MENU")) && (v = field(v, "LABEL"))) {
			if (g)
				gen_text(g, v);
		} else if ((v = field(line, "APPEND"))) {
			char *p = strstr(v, "init=");

			if (p)
				copy_word(cur_init, sizeof(cur_init), p + 5);
			if (g)
				gen_version(g, cur_init);
		}
	}
	if (g && !is_default)
		snprintf(init[num_gens++], sizeof(init[0]), "%s", cur_init);
	else if (is_default)
		snprintf(def_init, sizeof(def_init), "%s", cur_init);

	for (i = 0; i < num_gens; i++) {
		if (current_gen < 0 && def_init[0] && !strcmp(init[i], def_init)) {
			gens[i].current = true;
			current_gen = i;
		}
	}
	free(buf);
}

/* Main menu */

enum action { ACT_NIXOS, ACT_GENERATIONS, ACT_LINEAGE, ACT_RECOVERY, ACT_FASTBOOT, ACT_REBOOT };

static const struct item {
	const char *section;
	const char *label;
	enum action action;
	int y;
} items[] = {
	{ "NIXOS", "Boot", ACT_NIXOS, 58 },
	{ NULL, "Generations", ACT_GENERATIONS, 82 },
	{ "LINEAGEOS", "Boot", ACT_LINEAGE, 134 },
	{ NULL, "Recovery", ACT_RECOVERY, 158 },
	{ "TOOLS", "Fastboot", ACT_FASTBOOT, 210 },
	{ NULL, "Reboot", ACT_REBOOT, 234 },
};

static void draw_menu(int sel, int remaining_ms, const char *status)
{
	char buf[32];
	int i;

	ui_clear();
	ui_header("AGM M7", "U-Boot");
	for (i = 0; i < ARRAY_SIZE(items); i++) {
		const struct item *it = &items[i];
		bool s = i == sel;
		u32 fg = s ? COL_SEL_FG : COL_FG;

		if (it->section)
			ui_text(8, it->y - 22, it->section, font_small, COL_DIM, false);
		ui_sel_row(it->y, s);
		ui_text(20, it->y, it->label, font_small, fg, s);
		if (it->action == ACT_NIXOS && current_gen >= 0) {
			snprintf(buf, sizeof(buf), "gen %.*s", (int)strcspn(gens[current_gen].text, " "),
				 gens[current_gen].text);
			ui_text(168, it->y, buf, font_small, s ? COL_SEL_FG : COL_DIM, false);
		}
		if (it->action == ACT_GENERATIONS)
			ui_glyph(216, it->y, 0x10, font_small, s ? COL_SEL_FG : COL_DIM);
	}

	if (status) {
		ui_text(8, 266, status, font_small, COL_DIM, false);
	} else if (remaining_ms >= 0) {
		snprintf(buf, sizeof(buf), "NixOS in %d", (remaining_ms + 999) / 1000);
		ui_text(8, 266, buf, font_small, COL_DIM, false);
		ui_fill(8, 286, 224, 2, COL_LINE);
		ui_fill(8, 286, 224 * remaining_ms / COUNTDOWN_MS, 2, COL_ACCENT);
	}
	ui_footer("\x1e\x1f move    OK boot");
	ui_flush();
}

static int draw_generations(int sel, int top)
{
	int i;

	ui_clear();
	ui_header("NixOS generations", NULL);
	if (!num_gens)
		ui_text(8, 38, "no extlinux.conf", font_small, COL_DIM, false);
	for (i = 0; i < GEN_ROWS && top + i < num_gens; i++) {
		struct gen *g = &gens[top + i];
		bool s = top + i == sel;
		int y = 38 + i * 24;

		ui_sel_row(y, s);
		ui_text(8, y, g->text, font_small, s ? COL_SEL_FG : COL_FG, s);
		if (g->current)
			ui_text(168, y, "current", font_small, s ? COL_SEL_FG : COL_DIM, false);
	}
	if (sel < num_gens)
		ui_text(8, 240, gens[sel].version, font_small, COL_DIM, false);
	ui_footer("OK boot   BACK menu");
	ui_flush();

	return 0;
}

static void boot_status(int sel, const char *msg)
{
	draw_menu(sel, -1, msg);
}

static void boot_generation(int idx)
{
	env_set("pxe_label_override", gens[idx].label);
	run_command("run boot_nixos", 0);
	env_set("pxe_label_override", NULL);
}

static void generations(void)
{
	int sel = current_gen > 0 ? current_gen : 0, top = 0;

	for (;;) {
		if (sel < top)
			top = sel;
		if (sel >= top + GEN_ROWS)
			top = sel - GEN_ROWS + 1;
		draw_generations(sel, top);

		switch (wait_key()) {
		case K_UP:
			if (sel > 0)
				sel--;
			break;
		case K_DOWN:
			if (sel < num_gens - 1)
				sel++;
			break;
		case K_OK:
			if (sel < num_gens) {
				ui_clear();
				ui_header("NixOS generations", NULL);
				ui_text(8, 38, "booting", font_small, COL_DIM, false);
				ui_text(8, 62, gens[sel].text, font_small, COL_FG, true);
				ui_flush();
				boot_generation(sel);
			}
			return;
		case K_BACK:
			return;
		default:
			break;
		}
	}
}

/* Fastboot */

static struct {
	bool active;
	bool connected;
	bool busy;
	char cmd[16];
	char name[24];
	u64 total;
	ulong start;
	ulong last_draw;
	u64 last_done;
} fb;

static void fb_info_rows(void)
{
	ui_text(8, 200, "serial", font_small, COL_DIM, false);
	ui_text(72, 200, env_get("serial#") ?: "-", font_small, COL_FG, false);
	ui_text(8, 220, "buffer", font_small, COL_DIM, false);
	{
		char buf[16];

		snprintf(buf, sizeof(buf), "%d MiB", CONFIG_FASTBOOT_BUF_SIZE >> 20);
		ui_text(72, 220, buf, font_small, COL_FG, false);
	}
	ui_footer("BACK leave fastboot");
}

static void fb_draw_ready(void)
{
	ui_clear();
	ui_header("Fastboot", NULL);
	ui_text_center(88, "READY", font_big, COL_FG);
	ui_text_center(128, fb.connected ? "connected" : "waiting for host", font_small, COL_DIM);
	fb_info_rows();
	ui_flush();
}

static void fmt_mib(char *buf, size_t len, u64 bytes)
{
	u64 tenths = bytes * 10 / (1024 * 1024);

	snprintf(buf, len, "%llu.%llu", tenths / 10, tenths % 10);
}

static void fb_draw_progress(const char *phase, u64 done)
{
	char a[16], b[16], line[32];
	ulong ms = get_timer(fb.start);
	u64 pct = fb.total ? done * 100 / fb.total : 0;

	ui_clear();
	ui_header("Fastboot", NULL);
	ui_text(8, 64, phase, font_small, COL_DIM, false);
	ui_text(8, 84, fb.name, font_big, COL_FG, true);
	ui_fill(8, 140, 224, 16, COL_TRACK);
	ui_fill(8, 140, (int)(224 * pct / 100), 16, COL_ACCENT);
	fmt_mib(a, sizeof(a), done);
	fmt_mib(b, sizeof(b), fb.total);
	snprintf(line, sizeof(line), "%s / %s MiB", a, b);
	ui_text(8, 164, line, font_small, COL_FG, false);
	snprintf(line, sizeof(line), "%llu%%", pct);
	ui_text_right(232, 164, line, COL_FG);
	if (ms > 500 && done) {
		u64 rate = done * 1000 / ms;

		fmt_mib(a, sizeof(a), rate);
		snprintf(line, sizeof(line), "%s MB/s", a);
		ui_text(8, 188, line, font_small, COL_DIM, false);
		snprintf(line, sizeof(line), "%llu s left", (fb.total - done) / (rate ?: 1));
		ui_text_right(232, 188, line, COL_DIM);
	}
	ui_footer("do not unplug");
	ui_flush();
	fb.last_draw = get_timer(0);
	fb.last_done = done;
}

static void fb_draw_result(const char *response)
{
	bool ok = !strncmp(response, "OKAY", 4);
	char line[32];

	ui_clear();
	ui_header("Fastboot", NULL);
	ui_text_center(88, ok ? "OKAY" : "FAIL", font_big, ok ? COL_OK : COL_FAIL);
	snprintf(line, sizeof(line), "%s %s", fb.cmd, fb.name);
	ui_text_center(128, line, font_small, COL_DIM);
	if (ok && fb.total) {
		char a[16];
		ulong ms = get_timer(fb.start);

		fmt_mib(a, sizeof(a), fb.total);
		snprintf(line, sizeof(line), "%s MiB in %lu.%lu s", a, ms / 1000, ms % 1000 / 100);
		ui_text_center(148, line, font_small, COL_DIM);
	} else if (!ok) {
		snprintf(line, sizeof(line), "%.28s", response + 4);
		ui_text_center(148, line, font_small, COL_FAIL);
	}
	fb_info_rows();
	ui_flush();
}

void fastboot_ui_update(enum fastboot_ui_event event, const char *s, u64 done, u64 total)
{
	if (!fb.active)
		return;

	switch (event) {
	case FASTBOOT_UI_COMMAND:
		snprintf(fb.cmd, sizeof(fb.cmd), "%s", s);
		if (!fb.connected) {
			fb.connected = true;
			if (!fb.busy)
				fb_draw_ready();
		}
		break;
	case FASTBOOT_UI_DOWNLOAD:
		if (!done) {
			fb.busy = true;
			fb.total = total;
			fb.start = get_timer(0);
			snprintf(fb.name, sizeof(fb.name), "image");
			fb_draw_progress("downloading", 0);
		} else if (done == total || get_timer(fb.last_draw) > 200) {
			fb_draw_progress("downloading", done);
		}
		break;
	case FASTBOOT_UI_FLASH:
		if (s) {
			snprintf(fb.name, sizeof(fb.name), "%.14s", s);
			fb.busy = true;
			fb.total = total;
			fb_draw_progress("writing", 0);
		} else if (get_timer(fb.last_draw) > 200) {
			fb.total = total;
			fb_draw_progress("writing", done);
		}
		break;
	case FASTBOOT_UI_RESULT:
		if (!strncmp(s, "DATA", 4) || !strncmp(s, "INFO", 4))
			break;
		if (!strcmp(fb.cmd, "flash") || !strcmp(fb.cmd, "erase") ||
		    !strncmp(fb.cmd, "oem", 3) || !strncmp(s, "FAIL", 4)) {
			if (strcmp(fb.cmd, "flash") && strcmp(fb.cmd, "erase"))
				fb.name[0] = '\0';
			fb_draw_result(s);
			fb.busy = false;
			fb.total = 0;
		}
		break;
	}
}

static void fastboot(void)
{
	struct udevice *udc;

	memset(&fb, 0, sizeof(fb));
	fb.active = true;
	fb_draw_ready();

	if (udc_device_get_by_index(0, &udc)) {
		ui_text_center(160, "USB init failed", font_small, COL_FAIL);
		ui_flush();
		wait_key();
		fb.active = false;
		return;
	}
	fastboot_init(NULL, 0);
	g_dnl_clear_detach();
	if (!g_dnl_register("usb_dnl_fastboot")) {
		while (!g_dnl_detach()) {
			if (read_key() == K_BACK)
				break;
			schedule();
			dm_usb_gadget_handle_interrupts(udc);
		}
		g_dnl_unregister();
	}
	g_dnl_clear_detach();
	udc_device_put(udc);
	fb.active = false;
}

static void run_action(int sel)
{
	switch (items[sel].action) {
	case ACT_NIXOS:
		boot_status(sel, "booting NixOS");
		run_command("run boot_nixos", 0);
		break;
	case ACT_GENERATIONS:
		generations();
		return;
	case ACT_LINEAGE:
		boot_status(sel, "booting LineageOS");
		run_command("run boot_lineage", 0);
		break;
	case ACT_RECOVERY:
		boot_status(sel, "booting recovery");
		run_command("run boot_lineage_recovery", 0);
		break;
	case ACT_FASTBOOT:
		fastboot();
		return;
	case ACT_REBOOT:
		boot_status(sel, "rebooting");
		do_reset(NULL, 0, 0, NULL);
		return;
	}
	boot_status(sel, "boot failed");
	mdelay(2000);
}

static int do_agmmenu(struct cmd_tbl *cmdtp, int flag, int argc, char *const argv[])
{
	ulong start;
	int sel = 0, remaining = COUNTDOWN_MS;
	enum key k;

	if (uclass_first_device_err(UCLASS_VIDEO, &vid))
		return CMD_RET_FAILURE;
	font_small = &fonts[0];
	font_big = fonts[1].name ? &fonts[1] : &fonts[0];

	/* U-Boot's own output would draw over the menu. */
	env_set("stdout", "serial");
	env_set("stderr", "serial");

	load_generations();

	start = get_timer(0);
	draw_menu(sel, remaining, NULL);
	while (remaining > 0) {
		int left = COUNTDOWN_MS - (int)get_timer(start);

		k = read_key();
		if (k != K_NONE) {
			remaining = -1;
			break;
		}
		if (left / 100 != remaining / 100) {
			remaining = max(left, 0);
			draw_menu(sel, remaining, NULL);
		}
		schedule();
	}
	if (!remaining)
		run_action(0);

	for (;;) {
		draw_menu(sel, -1, NULL);
		k = wait_key();
		if (k == K_UP && sel > 0)
			sel--;
		else if (k == K_DOWN && sel < ARRAY_SIZE(items) - 1)
			sel++;
		else if (k == K_OK)
			run_action(sel);
	}

	return CMD_RET_SUCCESS;
}

U_BOOT_CMD(agmmenu, 1, 0, do_agmmenu, "AGM M7 boot menu", "");
