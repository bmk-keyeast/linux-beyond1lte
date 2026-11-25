// SPDX-License-Identifier: GPL-2.0
/*
 * Samsung S6E3HA9 "beyond1/beyond2 A3 S0" panels of the Galaxy S10 and S10+
 * (1440x3040 AMOLED, MIPI DSI command mode, 4 lanes).
 *
 * Like the S10e's panel-samsung-s6e3fa7-beyond0.c: the bootloader lights the
 * panel and the DSI link, nothing on mainline can power-cycle it, so it is
 * taken over lit. Off is DISPLAY_OFF + SLEEP_IN, on is SLEEP_OUT, the scan
 * geometry again, then DISPLAY_ON; reset and supplies are never touched.
 * Brightness is AOR (AMOLED off ratio) at the gamma the bootloader set.
 *
 * Command bytes and order, the brightness points and AOR values (Samsung's
 * GPL beyond1 A3 S0 panel tables) are those of butztill/s10-linux
 * (kernel/overlay/drivers/gpu/drm/tiny/exynos9820-decon.c, GPL-2.0-only),
 * where they lit, dimmed and power-cycled the S10's panel.
 *
 * The S10+ panel takes the same commands (Samsung's s6e3ha9_beyond2_a3_s0_panel.h
 * has the same scaler, CASET/PASET and init order as beyond1, and the same
 * brightness steps, beyond2_a3_s0_brt_tbl); only its AOR values differ
 * (beyond2_a3_s0_aor_table, LineageOS android_kernel_samsung_exynos9820).
 */

#include <linux/backlight.h>
#include <linux/cleanup.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>

#include <video/mipi_display.h>

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>
#include <drm/drm_probe_helper.h>

#define MAX_BRIGHTNESS	255

struct beyond1_variant {
	const u16 *aor_values;	/* ARRAY_SIZE(beyond1_brightness_points) of them */
	u16 width_mm, height_mm;
};

struct beyond1_panel {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct backlight_device *bl;
	const struct beyond1_variant *variant;
	struct mutex lock;	/* the panel state against the backlight */
	bool asleep;		/* SLEEP_IN sent; the bootloader leaves it awake */
	bool display_on;
};

static inline struct beyond1_panel *to_beyond1(struct drm_panel *panel)
{
	return container_of(panel, struct beyond1_panel, panel);
}

/* level 1 key around the standard DCS commands, level 2 (after 1) around vendor ones */
#define key1_on(ctx)	mipi_dsi_dcs_write_seq_multi(ctx, 0x9f, 0xa5, 0xa5)
#define key1_off(ctx)	mipi_dsi_dcs_write_seq_multi(ctx, 0x9f, 0x5a, 0x5a)
#define key2_on(ctx)	do { key1_on(ctx); mipi_dsi_dcs_write_seq_multi(ctx, 0xf0, 0x5a, 0x5a); } while (0)
#define key2_off(ctx)	do { mipi_dsi_dcs_write_seq_multi(ctx, 0xf0, 0xa5, 0xa5); key1_off(ctx); } while (0)

static const u8 beyond1_brightness_points[] = {
	0, 7, 14, 21, 28, 35, 36, 38, 40, 42, 44, 46, 48, 50, 52, 54,
	56, 57, 59, 61, 63, 65, 67, 69, 70, 72, 74, 76, 78, 80, 82, 84,
	86, 88, 90, 92, 94, 96, 98, 100, 102, 104, 106, 108, 110, 112,
	114, 116, 118, 120, 122, 124, 126, 128, 135, 142, 149, 157, 165,
	174, 183, 193, 201, 210, 219, 223, 227, 230, 234, 238, 242, 246,
	250, 255,
};

/* S10 (beyond1_a3_s0_aor_table) */
static const u16 beyond1_aor_values[] = {
	0x0bc9, 0x0bb0, 0x0b93, 0x0b77, 0x0b5f, 0x0b41, 0x0b20, 0x0b0a,
	0x0ae2, 0x0ac1, 0x0a97, 0x0a72, 0x0a4f, 0x0a1e, 0x0a01, 0x09e0,
	0x09a1, 0x0982, 0x095e, 0x0940, 0x0901, 0x08e2, 0x08a3, 0x085f,
	0x083f, 0x0803, 0x07bd, 0x0752, 0x071f, 0x06d7, 0x066b, 0x0601,
	0x05aa, 0x053a, 0x04c1, 0x0476, 0x0476, 0x0476, 0x0476, 0x0476,
	0x0476, 0x0476, 0x0476, 0x0476, 0x0476, 0x0476, 0x0476, 0x0476,
	0x0476, 0x0476, 0x0476, 0x0476, 0x0476, 0x0463, 0x03b9, 0x0321,
	0x025f, 0x018f, 0x014f, 0x014f, 0x014f, 0x014f, 0x014f, 0x014f,
	0x014f, 0x014f, 0x0147, 0x0100, 0x00d2, 0x008d, 0x003a, 0x0014,
	0x0014, 0x0014,
};
static_assert(ARRAY_SIZE(beyond1_brightness_points) == ARRAY_SIZE(beyond1_aor_values));

/* S10+ (beyond2_a3_s0_aor_table), for the same brightness steps */
static const u16 beyond2_aor_values[] = {
	0x0bc1, 0x0bad, 0x0b8c, 0x0b72, 0x0b55, 0x0b35, 0x0b19, 0x0afa,
	0x0ada, 0x0abc, 0x0a95, 0x0a6e, 0x0a41, 0x0a1b, 0x09ff, 0x09da,
	0x099b, 0x097e, 0x0955, 0x0924, 0x08ff, 0x08d9, 0x089a, 0x0854,
	0x0832, 0x07f1, 0x07ad, 0x0741, 0x0701, 0x06ba, 0x065e, 0x05f0,
	0x0592, 0x0521, 0x04bb, 0x045d, 0x045d, 0x045d, 0x045d, 0x045d,
	0x045d, 0x045d, 0x045d, 0x045d, 0x045d, 0x045d, 0x045d, 0x045d,
	0x045d, 0x045d, 0x045d, 0x045d, 0x045d, 0x0442, 0x03bf, 0x0321,
	0x026a, 0x01b3, 0x0158, 0x0158, 0x0158, 0x0158, 0x0158, 0x0158,
	0x0158, 0x0158, 0x0145, 0x00f1, 0x00ba, 0x008a, 0x0029, 0x0014,
	0x0014, 0x0014,
};
static_assert(ARRAY_SIZE(beyond1_brightness_points) == ARRAY_SIZE(beyond2_aor_values));

static u16 beyond1_aor(const u16 *aor_values, int level)
{
	size_t i;

	level = clamp(level, 0, MAX_BRIGHTNESS);
	for (i = 1; i < ARRAY_SIZE(beyond1_brightness_points); i++) {
		int lo = beyond1_brightness_points[i - 1];
		int hi = beyond1_brightness_points[i];

		if (level <= hi)
			return aor_values[i - 1] +
			       (aor_values[i] - aor_values[i - 1]) *
			       (level - lo) / (hi - lo);
	}
	return aor_values[ARRAY_SIZE(beyond1_brightness_points) - 1];
}

static void beyond1_set_aor(struct mipi_dsi_multi_context *ctx, const u16 *aor_values, int level)
{
	u16 aor = beyond1_aor(aor_values, level);
	u8 b1[] = { 0xb1, aor >> 8, aor & 0xff };

	key2_on(ctx);
	mipi_dsi_dcs_write_buffer_multi(ctx, b1, sizeof(b1));
	mipi_dsi_dcs_write_seq_multi(ctx, 0xf7, 0x03);	/* gamma/AOR update */
	key2_off(ctx);
}

static int beyond1_prepare(struct drm_panel *panel)
{
	struct beyond1_panel *bp = to_beyond1(panel);
	struct mipi_dsi_multi_context ctx = { .dsi = bp->dsi };

	guard(mutex)(&bp->lock);
	if (!bp->asleep)
		return 0;

	/* stock init_cmdtbl after sleep out: scan geometry, TE */
	mipi_dsi_dcs_exit_sleep_mode_multi(&ctx);
	mipi_dsi_msleep(&ctx, 120);
	key2_on(&ctx);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0xba, 0x01, 0x26, 0x08, 0x08, 0xf3);	/* scaler */
	key2_off(&ctx);
	mipi_dsi_dcs_write_seq_multi(&ctx, MIPI_DCS_SET_COLUMN_ADDRESS, 0x00, 0x00, 0x05, 0x9f);
	mipi_dsi_dcs_write_seq_multi(&ctx, MIPI_DCS_SET_PAGE_ADDRESS, 0x00, 0x00, 0x0b, 0xdf);
	key1_on(&ctx);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x1a, 0x1f, 0x00, 0x00, 0x00, 0x00);	/* set area */
	mipi_dsi_dcs_set_tear_on_multi(&ctx, MIPI_DSI_DCS_TEAR_MODE_VBLANK);
	key1_off(&ctx);
	/* the panel's self mask comes back on with SLEEP_OUT; butztill keeps it off */
	key2_on(&ctx);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x7a, 0x00);
	key2_off(&ctx);
	beyond1_set_aor(&ctx, bp->variant->aor_values, backlight_get_brightness(bp->bl));

	if (!ctx.accum_err)
		bp->asleep = false;
	return ctx.accum_err;
}

static int beyond1_enable(struct drm_panel *panel)
{
	struct beyond1_panel *bp = to_beyond1(panel);
	struct mipi_dsi_multi_context ctx = { .dsi = bp->dsi };

	if (bp->display_on)
		return 0;

	key1_on(&ctx);
	mipi_dsi_dcs_set_display_on_multi(&ctx);
	key1_off(&ctx);

	if (!ctx.accum_err)
		bp->display_on = true;
	return ctx.accum_err;
}

static int beyond1_disable(struct drm_panel *panel)
{
	struct beyond1_panel *bp = to_beyond1(panel);
	struct mipi_dsi_multi_context ctx = { .dsi = bp->dsi };

	key1_on(&ctx);
	mipi_dsi_dcs_set_display_off_multi(&ctx);
	key1_off(&ctx);
	bp->display_on = false;
	return ctx.accum_err;
}

static int beyond1_unprepare(struct drm_panel *panel)
{
	struct beyond1_panel *bp = to_beyond1(panel);
	struct mipi_dsi_multi_context ctx = { .dsi = bp->dsi };

	guard(mutex)(&bp->lock);
	/* stock exit_cmdtbl */
	key1_on(&ctx);
	mipi_dsi_dcs_enter_sleep_mode_multi(&ctx);
	key1_off(&ctx);
	mipi_dsi_msleep(&ctx, 120);
	bp->asleep = true;
	return ctx.accum_err;
}

static const struct drm_display_mode beyond1_mode = {
	/* command mode: these only pace vblank */
	.clock = 1446 * 3059 * 60 / 1000,
	.hdisplay = 1440,
	.hsync_start = 1440 + 2,
	.hsync_end = 1440 + 2 + 2,
	.htotal = 1440 + 2 + 2 + 2,
	.vdisplay = 3040,
	.vsync_start = 3040 + 3,
	.vsync_end = 3040 + 3 + 1,
	.vtotal = 3040 + 3 + 1 + 15,
	.type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED,
};

static int beyond1_get_modes(struct drm_panel *panel,
			     struct drm_connector *connector)
{
	struct beyond1_panel *bp = to_beyond1(panel);
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(connector->dev, &beyond1_mode);
	if (!mode)
		return -ENOMEM;
	drm_mode_set_name(mode);
	mode->width_mm = bp->variant->width_mm;
	mode->height_mm = bp->variant->height_mm;
	connector->display_info.width_mm = mode->width_mm;
	connector->display_info.height_mm = mode->height_mm;
	drm_mode_probed_add(connector, mode);
	return 1;
}

static const struct drm_panel_funcs beyond1_panel_funcs = {
	.prepare = beyond1_prepare,
	.enable = beyond1_enable,
	.disable = beyond1_disable,
	.unprepare = beyond1_unprepare,
	.get_modes = beyond1_get_modes,
};

static int beyond1_bl_update_status(struct backlight_device *bl)
{
	struct beyond1_panel *bp = bl_get_data(bl);
	struct mipi_dsi_multi_context ctx = { .dsi = bp->dsi };

	guard(mutex)(&bp->lock);
	/* applied by prepare when the panel wakes up */
	if (bp->asleep)
		return 0;

	beyond1_set_aor(&ctx, bp->variant->aor_values, backlight_get_brightness(bl));
	return ctx.accum_err;
}

static const struct backlight_ops beyond1_bl_ops = {
	.update_status = beyond1_bl_update_status,
};

static int beyond1_probe(struct mipi_dsi_device *dsi)
{
	struct backlight_properties props = {
		.type = BACKLIGHT_RAW,
		.brightness = MAX_BRIGHTNESS / 2,
		.max_brightness = MAX_BRIGHTNESS,
		.scale = BACKLIGHT_SCALE_NON_LINEAR,
	};
	struct device *dev = &dsi->dev;
	struct beyond1_panel *bp;
	int ret;

	bp = devm_drm_panel_alloc(dev, struct beyond1_panel, panel,
				  &beyond1_panel_funcs, DRM_MODE_CONNECTOR_DSI);
	if (IS_ERR(bp))
		return PTR_ERR(bp);

	bp->dsi = dsi;
	bp->variant = of_device_get_match_data(dev);
	bp->display_on = true;
	ret = devm_mutex_init(dev, &bp->lock);
	if (ret)
		return ret;
	mipi_dsi_set_drvdata(dsi, bp);

	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS;

	bp->bl = devm_backlight_device_register(dev, dev_name(dev), dev, bp,
						&beyond1_bl_ops, &props);
	if (IS_ERR(bp->bl))
		return dev_err_probe(dev, PTR_ERR(bp->bl),
				     "failed to register backlight\n");
	bp->panel.backlight = bp->bl;

	drm_panel_add(&bp->panel);

	ret = mipi_dsi_attach(dsi);
	if (ret) {
		drm_panel_remove(&bp->panel);
		return dev_err_probe(dev, ret, "failed to attach to DSI host\n");
	}

	/* the lit panel shows the bootloader's AOR until told otherwise */
	backlight_update_status(bp->bl);
	return 0;
}

static void beyond1_remove(struct mipi_dsi_device *dsi)
{
	struct beyond1_panel *bp = mipi_dsi_get_drvdata(dsi);

	mipi_dsi_detach(dsi);
	drm_panel_remove(&bp->panel);
}

/* panel sizes: S10 66x140 mm (6.1"), S10+ 70x148 mm (6.4", vendor DT) */
static const struct beyond1_variant beyond1_s10 = {
	.aor_values = beyond1_aor_values, .width_mm = 66, .height_mm = 140,
};
static const struct beyond1_variant beyond2_s10plus = {
	.aor_values = beyond2_aor_values, .width_mm = 70, .height_mm = 148,
};

static const struct of_device_id beyond1_of_match[] = {
	{ .compatible = "samsung,s6e3ha9-beyond1", .data = &beyond1_s10 },
	{ .compatible = "samsung,s6e3ha9-beyond2", .data = &beyond2_s10plus },
	{ }
};
MODULE_DEVICE_TABLE(of, beyond1_of_match);

static struct mipi_dsi_driver beyond1_driver = {
	.probe = beyond1_probe,
	.remove = beyond1_remove,
	.driver = {
		.name = "panel-samsung-s6e3ha9-beyond1",
		.of_match_table = beyond1_of_match,
	},
};
module_mipi_dsi_driver(beyond1_driver);

MODULE_DESCRIPTION("Samsung S6E3HA9 panels of the Galaxy S10 and S10+ (bootloader handover)");
MODULE_LICENSE("GPL");
