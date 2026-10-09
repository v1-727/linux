// SPDX-License-Identifier: GPL-2.0-only
/*
 * Novatek NT35596 1080x1920 video mode panels of the Asus ZE551ML/ZX550ML
 * family, and their backlight.
 *
 * The glass comes from TM or AUO; firmware tells which through the LCD ID
 * (the "asus,lcd-id" property, defaulting to TM). Both run from the PMIC's
 * VPROG3 rail and a reset GPIO. The backlight is a SoC PWM, gated by an
 * enable GPIO.
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/io.h>
#include <linux/pci.h>
#include <linux/pm_runtime.h>
#include <linux/property.h>

#include <linux/platform_data/x86/intel_scu_ipc.h>

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_print.h>

#include "mrfld_drv.h"

/* LCD IDs reported by the Asus firmware */
#define ASUS_LCD_ID_NT_AUO		0
#define ASUS_LCD_ID_NT_TM		3

/* PMIC VPROG3: the panel's 2.85 V supply */
#define PMIC_VPROG3_CTRL		0xae
#define   PMIC_VPROG_EN			BIT(0)

/* Backlight PWM, the fourth PWM block next to the 00:17.0 one */
#define BL_PWM_BASE			0xff013c00
#define BL_PWM_SIZE			0x400
#define   BL_PWM_ENABLE			BIT(31)
#define   BL_PWM_SW_UPDATE		BIT(30)
#define   BL_PWM_BASE_UNIT		(0x1555 << 8)	/* 25 kHz */
#define BL_PWM_PCI_DEVFN		PCI_DEVFN(0x17, 0)
#define BL_MAX_BRIGHTNESS		255

static const struct drm_display_mode nt35596_mode = {
	.clock = 138265,
	.hdisplay = 1080,
	.hsync_start = 1080 + 90,
	.hsync_end = 1080 + 90 + 8,
	.htotal = 1080 + 90 + 8 + 16,
	.vdisplay = 1920,
	.vsync_start = 1920 + 4,
	.vsync_end = 1920 + 4 + 2,
	.vtotal = 1920 + 4 + 2 + 4,
	.width_mm = 68,
	.height_mm = 121,
	.type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED,
};

static void nt35596_reset_pulse(struct mrfld_device *mrfld, unsigned int settle_ms)
{
	/* reset-gpios is active low: 1 holds the panel in reset */
	gpiod_set_value_cansleep(mrfld->reset_gpio, 0);
	usleep_range(5000, 5100);
	gpiod_set_value_cansleep(mrfld->reset_gpio, 1);
	usleep_range(5000, 5100);
	gpiod_set_value_cansleep(mrfld->reset_gpio, 0);
	msleep(settle_ms);
}

static int nt35596_tm_prepare(struct mrfld_device *mrfld)
{
	struct mipi_dsi_multi_context ctx = { .dsi = mrfld->dsi };

	if (!mrfld->panel_powered_once) {
		/* two pulses, to recover panels left in a bad state */
		nt35596_reset_pulse(mrfld, 120);
		nt35596_reset_pulse(mrfld, 20);
	} else {
		nt35596_reset_pulse(mrfld, 20);
	}

	mipi_dsi_dcs_write_seq_multi(&ctx, 0xff, 0x05);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0xfb, 0x01);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0xc5, 0x31);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0xff, 0x00);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0xfb, 0x01);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x35, 0x01);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0xd3, 0x06);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0xd4, 0x04);
	mipi_dsi_dcs_exit_sleep_mode_multi(&ctx);
	mipi_dsi_msleep(&ctx, 15);

	return ctx.accum_err;
}

static int nt35596_auo_prepare(struct mrfld_device *mrfld)
{
	struct mipi_dsi_multi_context ctx = { .dsi = mrfld->dsi };

	nt35596_reset_pulse(mrfld, mrfld->panel_powered_once ? 60 : 120);

	mipi_dsi_dcs_write_seq_multi(&ctx, 0xff, 0x00);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0xd3, 0x06);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0xd4, 0x04);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0xff, 0x05);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0xfb, 0x01);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x05, 0x1a);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x06, 0x10);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x07, 0x00);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x15, 0x1a);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x16, 0x10);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x17, 0x10);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x53, 0x06);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x7e, 0x05);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x7f, 0x20);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x86, 0x1b);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x87, 0x39);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x88, 0x1b);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x89, 0x39);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0xb5, 0x20);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x8c, 0x01);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x4d, 0x00);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x4e, 0x00);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x4f, 0x11);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x50, 0x11);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x54, 0x70);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0xff, 0x00);
	mipi_dsi_dcs_exit_sleep_mode_multi(&ctx);
	mipi_dsi_msleep(&ctx, 15);

	return ctx.accum_err;
}

static int nt35596_enable(struct mrfld_device *mrfld)
{
	struct mipi_dsi_multi_context ctx = { .dsi = mrfld->dsi };

	/* Give the panel 100 ms of video before turning the display on */
	msleep(100);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0xff, 0x00);
	mipi_dsi_dcs_exit_sleep_mode_multi(&ctx);
	mipi_dsi_msleep(&ctx, 10);
	mipi_dsi_dcs_set_display_on_multi(&ctx);

	return ctx.accum_err;
}

static void nt35596_disable(struct mrfld_device *mrfld)
{
	struct mipi_dsi_multi_context ctx = { .dsi = mrfld->dsi };

	mipi_dsi_dcs_set_display_off_multi(&ctx);
	mipi_dsi_dcs_enter_sleep_mode_multi(&ctx);
	mipi_dsi_usleep_range(&ctx, 1000, 1500);
	/* deep standby */
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x4f, 0x01);
	mipi_dsi_msleep(&ctx, 50);

	if (ctx.accum_err)
		drm_warn(&mrfld->drm, "panel did not go to sleep: %d\n", ctx.accum_err);
}

static const struct mrfld_panel_desc nt35596_tm = {
	.name = "nt35596-tm",
	.mode = &nt35596_mode,
	.width_mm = 68,
	.height_mm = 121,
	.lanes = 4,
	.dphy_param = 0x3f1f7817,
	.high_low_switch_count = 0x31,
	.clk_lane_switch_time = 0x3e0018,
	.lp_byteclk = 0x6,
	.prepare = nt35596_tm_prepare,
	.enable = nt35596_enable,
	.disable = nt35596_disable,
};

static const struct mrfld_panel_desc nt35596_auo = {
	.name = "nt35596-auo",
	.mode = &nt35596_mode,
	.width_mm = 68,
	.height_mm = 121,
	.lanes = 4,
	.dphy_param = 0x351b6d1f,
	.high_low_switch_count = 0x2f,
	.clk_lane_switch_time = 0x3d0016,
	.lp_byteclk = 0x6,
	.prepare = nt35596_auo_prepare,
	.enable = nt35596_enable,
	.disable = nt35596_disable,
};

/* Power the panel up and reset it; the DSI link must be up, in LP mode */
int mrfld_panel_power_on(struct mrfld_device *mrfld)
{
	int ret;

	ret = intel_scu_ipc_dev_update(mrfld->scu, PMIC_VPROG3_CTRL,
				       PMIC_VPROG_EN, PMIC_VPROG_EN);
	if (ret) {
		drm_err(&mrfld->drm, "panel supply did not turn on: %d\n", ret);
		return ret;
	}
	usleep_range(15000, 15100);

	ret = mrfld->panel->prepare(mrfld);
	mrfld->panel_powered_once = true;
	return ret;
}

static void mrfld_backlight_write(struct mrfld_device *mrfld, unsigned int level)
{
	u32 val = BL_PWM_SW_UPDATE | BL_PWM_BASE_UNIT | (~level & 0xff);

	if (level)
		val |= BL_PWM_ENABLE;
	writel(val, mrfld->pwm);
}

static int mrfld_backlight_update_status(struct backlight_device *bd)
{
	struct mrfld_device *mrfld = bl_get_data(bd);
	unsigned int level = backlight_get_brightness(bd);

	mrfld_backlight_write(mrfld, level);
	gpiod_set_value_cansleep(mrfld->backlight_gpio, level ? 1 : 0);
	return 0;
}

static const struct backlight_ops mrfld_backlight_ops = {
	.options = BL_CORE_SUSPENDRESUME,
	.update_status = mrfld_backlight_update_status,
};

void mrfld_backlight_set(struct mrfld_device *mrfld, bool on)
{
	if (on)
		backlight_enable(mrfld->backlight);
	else
		backlight_disable(mrfld->backlight);
}

static void mrfld_panel_release_pwm(void *data)
{
	struct pci_dev *pwm_pdev = data;

	pm_runtime_put(&pwm_pdev->dev);
	pci_dev_put(pwm_pdev);
}

int mrfld_panel_init(struct mrfld_device *mrfld)
{
	struct device *dev = &mrfld->pdev->dev;
	struct backlight_properties props = {
		.type = BACKLIGHT_RAW,
		.max_brightness = BL_MAX_BRIGHTNESS,
		.brightness = BL_MAX_BRIGHTNESS,
		.power = BACKLIGHT_POWER_ON,
	};
	u32 lcd_id = ASUS_LCD_ID_NT_TM;
	int ret;

	device_property_read_u32(dev, "asus,lcd-id", &lcd_id);
	switch (lcd_id) {
	case ASUS_LCD_ID_NT_TM:
		mrfld->panel = &nt35596_tm;
		break;
	case ASUS_LCD_ID_NT_AUO:
		mrfld->panel = &nt35596_auo;
		break;
	default:
		return dev_err_probe(dev, -ENODEV, "unsupported LCD ID %u\n", lcd_id);
	}
	drm_info(&mrfld->drm, "panel %s\n", mrfld->panel->name);

	mrfld->scu = devm_intel_scu_ipc_dev_get(dev);
	if (!mrfld->scu)
		return -EPROBE_DEFER;

	mrfld->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(mrfld->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(mrfld->reset_gpio), "no panel reset GPIO\n");

	/* firmware left the panel lit: keep the backlight on until we take over */
	mrfld->backlight_gpio = devm_gpiod_get(dev, "backlight", GPIOD_OUT_HIGH);
	if (IS_ERR(mrfld->backlight_gpio))
		return dev_err_probe(dev, PTR_ERR(mrfld->backlight_gpio),
				     "no backlight enable GPIO\n");

	mrfld->pwm = devm_ioremap(dev, BL_PWM_BASE, BL_PWM_SIZE);
	if (!mrfld->pwm)
		return -ENOMEM;

	/* The backlight PWM lives in the power island of the 00:17.0 PWM */
	mrfld->pwm_pdev = pci_get_domain_bus_and_slot(0, 0, BL_PWM_PCI_DEVFN);
	if (mrfld->pwm_pdev) {
		pm_runtime_get_sync(&mrfld->pwm_pdev->dev);
		pci_set_power_state(mrfld->pwm_pdev, PCI_D0);
		ret = devm_add_action_or_reset(dev, mrfld_panel_release_pwm, mrfld->pwm_pdev);
		if (ret)
			return ret;
	} else {
		drm_warn(&mrfld->drm, "no PWM device, backlight may stay dark\n");
	}

	mrfld->backlight = devm_backlight_device_register(dev, "mrfld_backlight", dev, mrfld,
							  &mrfld_backlight_ops, &props);
	if (IS_ERR(mrfld->backlight))
		return PTR_ERR(mrfld->backlight);

	return 0;
}
