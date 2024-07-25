// SPDX-License-Identifier: GPL-2.0
/* copyright (C) 2019 Bytes at Work - http://www.bytesatwork.ch
 *
 * based on panel-raydium-rm68200.c
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/regulator/consumer.h>

#include <video/mipi_display.h>

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>
#include <drm/drm_print.h>

enum ili9806e_op {
	ILI9806C_SWITCH_PAGE,
	ILI9806C_COMMAND,
};

struct ili9806e_instr {
	enum ili9806e_op        op;

	union arg {
		struct cmd {
			u8      cmd;
			u8      data;
		} cmd;
		u8      page;
	} arg;
};

struct ili9806e {
	struct device *dev;
	struct drm_panel panel;
	struct gpio_desc *reset_gpio;
	struct regulator *supply;
	struct backlight_device *backlight;
	bool prepared;
	bool enabled;
};

#define ILI9806C_SWITCH_PAGE_INSTR(_page)	\
	{					\
		.op = ILI9806C_SWITCH_PAGE,	\
		.arg = {			\
			.page = (_page),	\
		},				\
	}

#define ILI9806C_COMMAND_INSTR(_cmd, _data)		\
	{						\
		.op = ILI9806C_COMMAND,		\
		.arg = {				\
			.cmd = {			\
				.cmd = (_cmd),		\
				.data = (_data),	\
			},				\
		},					\
	}

static const struct ili9806e_instr ili9806e_init[] = {
	ILI9806C_SWITCH_PAGE_INSTR(1),
	ILI9806C_COMMAND_INSTR(0x08, 0x10), //Output SDA
	ILI9806C_COMMAND_INSTR(0x20, 0x00), //set DE/VSYNC mode
	ILI9806C_COMMAND_INSTR(0x21, 0x01), //DE = 1 Active
	ILI9806C_COMMAND_INSTR(0x30, 0x01), //Resolution setting 480 X 854
	ILI9806C_COMMAND_INSTR(0x31, 0x00), //Inversion setting 2-dot
	ILI9806C_COMMAND_INSTR(0x40, 0x16), //BT  AVDD,AVDD
	ILI9806C_COMMAND_INSTR(0x41, 0x33),
	ILI9806C_COMMAND_INSTR(0x42, 0x03), //VGL=DDVDH+VCIP -DDVDL,VGH=2DDVDL-VCIP
	ILI9806C_COMMAND_INSTR(0x43, 0x09), //SET VGH clamp level
	ILI9806C_COMMAND_INSTR(0x44, 0x06), //SET VGL clamp level
	ILI9806C_COMMAND_INSTR(0x50, 0x88), //VREG1
	ILI9806C_COMMAND_INSTR(0x51, 0x88), //VREG2
	ILI9806C_COMMAND_INSTR(0x52, 0x00), //Flicker MSB
	ILI9806C_COMMAND_INSTR(0x53, 0x49), //Flicker LSB
	ILI9806C_COMMAND_INSTR(0x55, 0x49), //Flicker
	ILI9806C_COMMAND_INSTR(0x60, 0x07),
	ILI9806C_COMMAND_INSTR(0x61, 0x00),
	ILI9806C_COMMAND_INSTR(0x62, 0x07),
	ILI9806C_COMMAND_INSTR(0x63, 0x00),
	ILI9806C_COMMAND_INSTR(0xA0, 0x00), //Positive Gamma
	ILI9806C_COMMAND_INSTR(0xA1, 0x09),
	ILI9806C_COMMAND_INSTR(0xA2, 0x11),
	ILI9806C_COMMAND_INSTR(0xA3, 0x0B),
	ILI9806C_COMMAND_INSTR(0xA4, 0x05),
	ILI9806C_COMMAND_INSTR(0xA5, 0x08),
	ILI9806C_COMMAND_INSTR(0xA6, 0x06),
	ILI9806C_COMMAND_INSTR(0xA7, 0x04),
	ILI9806C_COMMAND_INSTR(0xA8, 0x09),
	ILI9806C_COMMAND_INSTR(0xA9, 0x0C),
	ILI9806C_COMMAND_INSTR(0xAA, 0x15),
	ILI9806C_COMMAND_INSTR(0xAB, 0x08),
	ILI9806C_COMMAND_INSTR(0xAC, 0x0F),
	ILI9806C_COMMAND_INSTR(0xAD, 0x12),
	ILI9806C_COMMAND_INSTR(0xAE, 0x09),
	ILI9806C_COMMAND_INSTR(0xAF, 0x00),
	ILI9806C_COMMAND_INSTR(0xC0, 0x00), //Negative Gamma
	ILI9806C_COMMAND_INSTR(0xC1, 0x09),
	ILI9806C_COMMAND_INSTR(0xC2, 0x10),
	ILI9806C_COMMAND_INSTR(0xC3, 0x0C),
	ILI9806C_COMMAND_INSTR(0xC4, 0x05),
	ILI9806C_COMMAND_INSTR(0xC5, 0x08),
	ILI9806C_COMMAND_INSTR(0xC6, 0x06),
	ILI9806C_COMMAND_INSTR(0xC7, 0x04),
	ILI9806C_COMMAND_INSTR(0xC8, 0x08),
	ILI9806C_COMMAND_INSTR(0xC9, 0x0C),
	ILI9806C_COMMAND_INSTR(0xCA, 0x14),
	ILI9806C_COMMAND_INSTR(0xCB, 0x08),
	ILI9806C_COMMAND_INSTR(0xCC, 0x0F),
	ILI9806C_COMMAND_INSTR(0xCD, 0x11),
	ILI9806C_COMMAND_INSTR(0xCE, 0x09),
	ILI9806C_COMMAND_INSTR(0xCF, 0x00),
	ILI9806C_SWITCH_PAGE_INSTR(6),
	ILI9806C_COMMAND_INSTR(0x00, 0x20),
	ILI9806C_COMMAND_INSTR(0x01, 0x0A),
	ILI9806C_COMMAND_INSTR(0x02, 0x00),
	ILI9806C_COMMAND_INSTR(0x03, 0x00),
	ILI9806C_COMMAND_INSTR(0x04, 0x01),
	ILI9806C_COMMAND_INSTR(0x05, 0x01),
	ILI9806C_COMMAND_INSTR(0x06, 0x98),
	ILI9806C_COMMAND_INSTR(0x07, 0x06),
	ILI9806C_COMMAND_INSTR(0x08, 0x01),
	ILI9806C_COMMAND_INSTR(0x09, 0x80),
	ILI9806C_COMMAND_INSTR(0x0A, 0x00),
	ILI9806C_COMMAND_INSTR(0x0B, 0x00),
	ILI9806C_COMMAND_INSTR(0x0C, 0x01),
	ILI9806C_COMMAND_INSTR(0x0D, 0x01),
	ILI9806C_COMMAND_INSTR(0x0E, 0x05),
	ILI9806C_COMMAND_INSTR(0x0F, 0x00),
	ILI9806C_COMMAND_INSTR(0x10, 0xF0),
	ILI9806C_COMMAND_INSTR(0x11, 0xF4),
	ILI9806C_COMMAND_INSTR(0x12, 0x01),
	ILI9806C_COMMAND_INSTR(0x13, 0x00),
	ILI9806C_COMMAND_INSTR(0x14, 0x00),
	ILI9806C_COMMAND_INSTR(0x15, 0xC0),
	ILI9806C_COMMAND_INSTR(0x16, 0x08),
	ILI9806C_COMMAND_INSTR(0x17, 0x00),
	ILI9806C_COMMAND_INSTR(0x18, 0x00),
	ILI9806C_COMMAND_INSTR(0x19, 0x00),
	ILI9806C_COMMAND_INSTR(0x1A, 0x00),
	ILI9806C_COMMAND_INSTR(0x1B, 0x00),
	ILI9806C_COMMAND_INSTR(0x1C, 0x00),
	ILI9806C_COMMAND_INSTR(0x1D, 0x00),
	ILI9806C_COMMAND_INSTR(0x20, 0x01),
	ILI9806C_COMMAND_INSTR(0x21, 0x23),
	ILI9806C_COMMAND_INSTR(0x22, 0x45),
	ILI9806C_COMMAND_INSTR(0x23, 0x67),
	ILI9806C_COMMAND_INSTR(0x24, 0x01),
	ILI9806C_COMMAND_INSTR(0x25, 0x23),
	ILI9806C_COMMAND_INSTR(0x26, 0x45),
	ILI9806C_COMMAND_INSTR(0x27, 0x67),
	ILI9806C_COMMAND_INSTR(0x30, 0x11),
	ILI9806C_COMMAND_INSTR(0x31, 0x11),
	ILI9806C_COMMAND_INSTR(0x32, 0x00),
	ILI9806C_COMMAND_INSTR(0x33, 0xEE),
	ILI9806C_COMMAND_INSTR(0x34, 0xFF),
	ILI9806C_COMMAND_INSTR(0x35, 0xBB),
	ILI9806C_COMMAND_INSTR(0x36, 0xAA),
	ILI9806C_COMMAND_INSTR(0x37, 0xDD),
	ILI9806C_COMMAND_INSTR(0x38, 0xCC),
	ILI9806C_COMMAND_INSTR(0x39, 0x66),
	ILI9806C_COMMAND_INSTR(0x3A, 0x77),
	ILI9806C_COMMAND_INSTR(0x3B, 0x22),
	ILI9806C_COMMAND_INSTR(0x3C, 0x22),
	ILI9806C_COMMAND_INSTR(0x3D, 0x22),
	ILI9806C_COMMAND_INSTR(0x3E, 0x22),
	ILI9806C_COMMAND_INSTR(0x3F, 0x22),
	ILI9806C_COMMAND_INSTR(0x40, 0x22),
	ILI9806C_SWITCH_PAGE_INSTR(7),
	ILI9806C_COMMAND_INSTR(0x17, 0x22),
	ILI9806C_COMMAND_INSTR(0x02, 0x77),
	ILI9806C_COMMAND_INSTR(0x26, 0xB2),
};

static const struct drm_display_mode default_mode = {
	.clock		= 48000,
	.hdisplay	= 480,
	.hsync_start	= 480 + 20,
	.hsync_end	= 480 + 20 + 4,
	.htotal		= 480 + 20 + 4 + 16,
	.vdisplay	= 854,
	.vsync_start	= 854 + 100,
	.vsync_end	= 854 + 100 + 10,
	.vtotal		= 854 + 100 + 10 + 50,
	.flags		= 0,
	.width_mm	= 87,
	.height_mm	= 87,
};

static inline struct ili9806e *panel_to_ili9806e(struct drm_panel *panel)
{
	return container_of(panel, struct ili9806e, panel);
}

/*
 * The panel seems to accept some private DCS commands that map
 * directly to registers.
 *
 * It is organised by page, with each page having its own set of
 * registers, and the first page looks like it's holding the standard
 * DCS commands.
 *
 * So before any attempt at sending a command or data, we have to be
 * sure if we're in the right page or not.
 */
static int ili9806e_switch_page(struct ili9806e *ctx, u8 page)
{
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);
	u8 buf[] = { 0xff, 0xff, 0x98, 0x06, 0x04, page };
	int ret;

	ret = mipi_dsi_dcs_write_buffer(dsi, buf, sizeof(buf));
	if (ret < 0)
		return ret;

	return 0;
}

static int ili9806e_send_cmd_data(struct ili9806e *ctx, u8 cmd, u8 data)
{
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);
	u8 buf[] = { cmd, data };
	int ret;

	ret = mipi_dsi_dcs_write_buffer(dsi, buf, sizeof(buf));
	if (ret < 0)
		return ret;

	return 0;
}


static int ili9806e_disable(struct drm_panel *panel)
{
	struct ili9806e *ctx = panel_to_ili9806e(panel);

	if (!ctx->enabled)
		return 0;

	backlight_disable(ctx->backlight);

	ctx->enabled = false;

	return 0;
}

static int ili9806e_unprepare(struct drm_panel *panel)
{
	struct ili9806e *ctx = panel_to_ili9806e(panel);
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);
	int ret;

	if (!ctx->prepared)
		return 0;

	ret = mipi_dsi_dcs_set_display_off(dsi);
	if (ret)
		DRM_WARN("failed to set display off: %d\n", ret);

	ret = mipi_dsi_dcs_enter_sleep_mode(dsi);
	if (ret)
		DRM_WARN("failed to enter sleep mode: %d\n", ret);

	msleep(120);

	if (ctx->reset_gpio) {
		gpiod_set_value_cansleep(ctx->reset_gpio, 1);
		msleep(20);
	}

	regulator_disable(ctx->supply);

	ctx->prepared = false;

	return 0;
}

static int ili9806e_prepare(struct drm_panel *panel)
{
	struct ili9806e *ctx = panel_to_ili9806e(panel);
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);
	int ret, i;

	if (ctx->prepared)
		return 0;

	ret = regulator_enable(ctx->supply);
	if (ret < 0) {
		DRM_ERROR("failed to enable supply: %d\n", ret);
		return ret;
	}

	if (ctx->reset_gpio) {
		gpiod_set_value_cansleep(ctx->reset_gpio, 1);
		msleep(20);
		gpiod_set_value_cansleep(ctx->reset_gpio, 0);
		msleep(100);
	}

	for (i = 0; i < ARRAY_SIZE(ili9806e_init); i++) {
		const struct ili9806e_instr *instr = &ili9806e_init[i];

		if (instr->op == ILI9806C_SWITCH_PAGE)
			ret = ili9806e_switch_page(ctx, instr->arg.page);
		else if (instr->op == ILI9806C_COMMAND)
			ret = ili9806e_send_cmd_data(ctx, instr->arg.cmd.cmd,
							instr->arg.cmd.data);

		if (ret)
			return ret;
	}

	ret = ili9806e_switch_page(ctx, 0);
	if (ret)
		return ret;


	ret = mipi_dsi_dcs_exit_sleep_mode(dsi);
	if (ret)
		return ret;

	msleep(125);

	ret = mipi_dsi_dcs_set_display_on(dsi);
	if (ret)
		return ret;

	msleep(20);

	ctx->prepared = true;

	return 0;
}

static int ili9806e_enable(struct drm_panel *panel)
{
	struct ili9806e *ctx = panel_to_ili9806e(panel);

	if (ctx->enabled)
		return 0;

	backlight_enable(ctx->backlight);

	ctx->enabled = true;

	return 0;
}

static int ili9806e_get_modes(struct drm_panel *panel,
			      struct drm_connector *connector)
{
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(connector->dev, &default_mode);
	if (!mode) {
		dev_err(panel->dev, "failed to add mode %ux%u@%u\n",
			default_mode.hdisplay, default_mode.vdisplay,
			drm_mode_vrefresh(&default_mode));
		return -ENOMEM;
	}

	drm_mode_set_name(mode);

	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	drm_mode_probed_add(connector, mode);

	connector->display_info.width_mm = mode->width_mm;
	connector->display_info.height_mm = mode->height_mm;

	return 1;
}

static const struct drm_panel_funcs ili9806e_drm_funcs = {
	.disable = ili9806e_disable,
	.unprepare = ili9806e_unprepare,
	.prepare = ili9806e_prepare,
	.enable = ili9806e_enable,
	.get_modes = ili9806e_get_modes,
};

static int ili9806e_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct ili9806e *ctx;
	int ret;

	ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->reset_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(ctx->reset_gpio)) {
		ret = PTR_ERR(ctx->reset_gpio);
		dev_err(dev, "cannot get reset GPIO: %d\n", ret);
		return ret;
	}

	ctx->supply = devm_regulator_get(dev, "power");
	if (IS_ERR(ctx->supply)) {
		ret = PTR_ERR(ctx->supply);
		dev_err(dev, "cannot get regulator: %d\n", ret);
		return ret;
	}

	ctx->backlight = devm_of_find_backlight(dev);
	if (IS_ERR(ctx->backlight))
		return PTR_ERR(ctx->backlight);

	mipi_dsi_set_drvdata(dsi, ctx);

	ctx->dev = dev;

	dsi->lanes = 2;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_BURST |
			  MIPI_DSI_MODE_LPM;

	drm_panel_init(&ctx->panel, dev, &ili9806e_drm_funcs,
		       DRM_MODE_CONNECTOR_DSI);

	drm_panel_add(&ctx->panel);

	ret = mipi_dsi_attach(dsi);
	if (ret < 0) {
		dev_err(dev, "mipi_dsi_attach() failed: %d\n", ret);
		drm_panel_remove(&ctx->panel);
		return ret;
	}

	return 0;
}

static void ili9806e_remove(struct mipi_dsi_device *dsi)
{
	struct ili9806e *ctx = mipi_dsi_get_drvdata(dsi);

	mipi_dsi_detach(dsi);
	drm_panel_remove(&ctx->panel);
}

static const struct of_device_id youritech_ili9806e_of_match[] = {
	{ .compatible = "youritech,ili9806" },
	{ }
};
MODULE_DEVICE_TABLE(of, youritech_ili9806e_of_match);

static struct mipi_dsi_driver youritech_ili9806e_driver = {
	.probe = ili9806e_probe,
	.remove = ili9806e_remove,
	.driver = {
		.name = "panel-youritech-ili9806",
		.of_match_table = youritech_ili9806e_of_match,
	},
};
module_mipi_dsi_driver(youritech_ili9806e_driver);

MODULE_AUTHOR("Stephan Dünner <stephan.duenner@bytesatwork.ch>");
MODULE_DESCRIPTION("DRM Driver for Youritech MIPI DSI panel with ILI9806 Controller");
MODULE_LICENSE("GPL");
