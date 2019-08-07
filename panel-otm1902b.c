// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2013, The Linux Foundation. All rights reserved.

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>
#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <video/mipi_display.h>

struct otm1902b {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct backlight_device *backlight;
	struct gpio_desc *reset_gpio;

	bool prepared;
	bool enabled;
};

static inline struct otm1902b *to_otm1902b(struct drm_panel *panel)
{
	return container_of(panel, struct otm1902b, panel);
}

#define dsi_generic_write_seq(dsi, seq...) do {				\
		static const u8 d[] = { seq };				\
		int ret;						\
		ret = mipi_dsi_generic_write(dsi, d, ARRAY_SIZE(d));	\
		if (ret < 0)						\
			return ret;					\
	} while (0)

#define dsi_dcs_write_seq(dsi, seq...) do {				\
		static const u8 d[] = { seq };				\
		int ret;						\
		ret = mipi_dsi_dcs_write_buffer(dsi, d, ARRAY_SIZE(d));	\
		if (ret < 0)						\
			return ret;					\
	} while (0)

static void otm1902b_reset(struct otm1902b *ctx)
{
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	msleep(20);
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	msleep(2);
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	msleep(10);
}

static int otm1902b_on(struct otm1902b *ctx)
{
	struct mipi_dsi_device *dsi = ctx->dsi;
	struct device *dev = &dsi->dev;
	int ret;

	dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	dsi_generic_write_seq(dsi, 0x00, 0x00);
	dsi_generic_write_seq(dsi, 0xff, 0x19, 0x02, 0x01, 0x00);
	msleep(1);
	dsi_generic_write_seq(dsi, 0x00, 0x80);
	msleep(1);
	dsi_generic_write_seq(dsi, 0xff, 0x19, 0x02);
	msleep(1);
	dsi_dcs_write_seq(dsi, 0x00, 0xb0);
	msleep(1);
	dsi_generic_write_seq(dsi, 0xca, 0xff, 0x02, 0x5f, 0x40);
	msleep(1);
	dsi_dcs_write_seq(dsi, MIPI_DCS_WRITE_CONTROL_DISPLAY, 0x2c);
	msleep(1);

	ret = mipi_dsi_dcs_exit_sleep_mode(dsi);
	if (ret < 0) {
		dev_err(dev, "Failed to exit sleep mode: %d\n", ret);
		return ret;
	}
	msleep(50);

	ret = mipi_dsi_dcs_set_display_on(dsi);
	if (ret < 0) {
		dev_err(dev, "Failed to set display on: %d\n", ret);
		return ret;
	}
	msleep(96);

	return 0;
}

static int otm1902b_off(struct otm1902b *ctx)
{
	struct mipi_dsi_device *dsi = ctx->dsi;
	struct device *dev = &dsi->dev;
	int ret;

	dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;

	ret = mipi_dsi_dcs_set_display_off(dsi);
	if (ret < 0) {
		dev_err(dev, "Failed to set display off: %d\n", ret);
		return ret;
	}
	msleep(50);

	ret = mipi_dsi_dcs_enter_sleep_mode(dsi);
	if (ret < 0) {
		dev_err(dev, "Failed to enter sleep mode: %d\n", ret);
		return ret;
	}
	msleep(100);

	return 0;
}

static int otm1902b_prepare(struct drm_panel *panel)
{
	struct otm1902b *ctx = to_otm1902b(panel);
	struct device *dev = &ctx->dsi->dev;
	int ret;

	if (ctx->prepared)
		return 0;

	otm1902b_reset(ctx);

	ret = otm1902b_on(ctx);
	if (ret < 0) {
		dev_err(dev, "Failed to initialize panel: %d\n", ret);
		gpiod_set_value_cansleep(ctx->reset_gpio, 0);
		return ret;
	}

	ctx->prepared = true;
	return 0;
}

static int otm1902b_unprepare(struct drm_panel *panel)
{
	struct otm1902b *ctx = to_otm1902b(panel);
	struct device *dev = &ctx->dsi->dev;
	int ret;

	if (!ctx->prepared)
		return 0;

	ret = otm1902b_off(ctx);
	if (ret < 0)
		dev_err(dev, "Failed to un-initialize panel: %d\n", ret);

	gpiod_set_value_cansleep(ctx->reset_gpio, 0);

	ctx->prepared = false;
	return 0;
}

static int otm1902b_enable(struct drm_panel *panel)
{
	struct otm1902b *ctx = to_otm1902b(panel);
	int ret;

	if (ctx->enabled)
		return 0;

	ret = backlight_enable(ctx->backlight);
	if (ret < 0) {
		dev_err(&ctx->dsi->dev, "Failed to enable backlight: %d\n", ret);
		return ret;
	}

	ctx->enabled = true;
	return 0;
}

static int otm1902b_disable(struct drm_panel *panel)
{
	struct otm1902b *ctx = to_otm1902b(panel);
	int ret;

	if (!ctx->enabled)
		return 0;

	ret = backlight_disable(ctx->backlight);
	if (ret < 0) {
		dev_err(&ctx->dsi->dev, "Failed to disable backlight: %d\n", ret);
		return ret;
	}

	ctx->enabled = false;
	return 0;
}

static const struct drm_display_mode otm1902b_mode = {
	.clock = (1080 + 144 + 12 + 32) * (1920 + 9 + 4 + 3) * 60 / 1000,
	.hdisplay = 1080,
	.hsync_start = 1080 + 144,
	.hsync_end = 1080 + 144 + 12,
	.htotal = 1080 + 144 + 12 + 32,
	.vdisplay = 1920,
	.vsync_start = 1920 + 9,
	.vsync_end = 1920 + 9 + 4,
	.vtotal = 1920 + 9 + 4 + 3,
	.vrefresh = 60,
	.width_mm = 62,
	.height_mm = 110,
};

static int otm1902b_get_modes(struct drm_panel *panel)
{
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(panel->drm, &otm1902b_mode);
	if (!mode)
		return -ENOMEM;

	drm_mode_set_name(mode);

	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	panel->connector->display_info.width_mm = mode->width_mm;
	panel->connector->display_info.height_mm = mode->height_mm;
	drm_mode_probed_add(panel->connector, mode);

	return 1;
}

static const struct drm_panel_funcs otm1902b_panel_funcs = {
	.disable = otm1902b_disable,
	.unprepare = otm1902b_unprepare,
	.prepare = otm1902b_prepare,
	.enable = otm1902b_enable,
	.get_modes = otm1902b_get_modes,
};

static int dsi_dcs_bl_get_brightness(struct backlight_device *bl)
{
	struct mipi_dsi_device *dsi = bl_get_data(bl);
	int ret;
	u16 brightness = bl->props.brightness;

	dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;

	ret = mipi_dsi_dcs_get_display_brightness(dsi, &brightness);
	if (ret < 0)
		return ret;

	dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	return brightness & 0xff;
}

static int dsi_dcs_bl_update_status(struct backlight_device *bl)
{
	struct mipi_dsi_device *dsi = bl_get_data(bl);
	int ret;

	dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;

	ret = mipi_dsi_dcs_set_display_brightness(dsi, bl->props.brightness);
	if (ret < 0)
		return ret;

	dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	return 0;
}

static const struct backlight_ops dsi_bl_ops = {
	.update_status = dsi_dcs_bl_update_status,
	.get_brightness = dsi_dcs_bl_get_brightness,
};

static struct backlight_device *
otm1902b_create_backlight(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct backlight_properties props;

	memset(&props, 0, sizeof(props));
	props.type = BACKLIGHT_RAW;
	props.brightness = 255;
	props.max_brightness = 255;

	return devm_backlight_device_register(dev, dev_name(dev), dev, dsi,
					      &dsi_bl_ops, &props);
}

static int otm1902b_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct otm1902b *ctx;
	int ret;

	ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(ctx->reset_gpio)) {
		ret = PTR_ERR(ctx->reset_gpio);
		dev_err(dev, "Failed to get reset-gpios: %d\n", ret);
		return ret;
	}

	ctx->backlight = otm1902b_create_backlight(dsi);
	if (IS_ERR(ctx->backlight)) {
		ret = PTR_ERR(ctx->backlight);
		dev_err(dev, "Failed to create backlight: %d\n", ret);
		return ret;
	}

	ctx->dsi = dsi;
	mipi_dsi_set_drvdata(dsi, ctx);

	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO_BURST | MIPI_DSI_MODE_VIDEO_HSE |
			  MIPI_DSI_MODE_EOT_PACKET |
			  MIPI_DSI_CLOCK_NON_CONTINUOUS;

	drm_panel_init(&ctx->panel);
	ctx->panel.dev = dev;
	ctx->panel.funcs = &otm1902b_panel_funcs;

	ret = drm_panel_add(&ctx->panel);
	if (ret < 0) {
		dev_err(dev, "Failed to add panel: %d\n", ret);
		return ret;
	}

	ret = mipi_dsi_attach(dsi);
	if (ret < 0) {
		dev_err(dev, "Failed to attach to DSI host: %d\n", ret);
		return ret;
	}

	return 0;
}

static int otm1902b_remove(struct mipi_dsi_device *dsi)
{
	struct otm1902b *ctx = mipi_dsi_get_drvdata(dsi);
	int ret;

	ret = mipi_dsi_detach(dsi);
	if (ret < 0)
		dev_err(&dsi->dev, "Failed to detach from DSI host: %d\n", ret);

	drm_panel_remove(&ctx->panel);

	return 0;
}

static const struct of_device_id otm1902b_of_match[] = {
	{ .compatible = "mdss,otm1902b" }, // FIXME
	{ }
};
MODULE_DEVICE_TABLE(of, otm1902b_of_match);

static struct mipi_dsi_driver otm1902b_driver = {
	.probe = otm1902b_probe,
	.remove = otm1902b_remove,
	.driver = {
		.name = "panel-otm1902b",
		.of_match_table = otm1902b_of_match,
	},
};
module_mipi_dsi_driver(otm1902b_driver);

MODULE_AUTHOR("linux-mdss-dsi-panel-driver-generator <fix@me>");
MODULE_DESCRIPTION("DRM driver for otm1902b 1080p cmd mode dsi panel");
MODULE_LICENSE("GPL v2");
