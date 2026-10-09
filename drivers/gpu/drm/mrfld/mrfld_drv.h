/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Intel Merrifield/Moorefield display controller driver
 */

#ifndef __MRFLD_DRV_H__
#define __MRFLD_DRV_H__

#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/types.h>

#include <drm/drm_connector.h>
#include <drm/drm_crtc.h>
#include <drm/drm_device.h>
#include <drm/drm_encoder.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_mm.h>
#include <drm/drm_modes.h>
#include <drm/drm_plane.h>

#include "mrfld_regs.h"

struct backlight_device;
struct gpio_desc;
struct intel_scu_ipc_dev;
struct mrfld_device;
struct pci_dev;

/**
 * struct mrfld_panel_desc - a DSI video mode panel
 * @name: panel name, also the name of its MIPI DSI device
 * @mode: the panel's only mode
 * @width_mm: active area width
 * @height_mm: active area height
 * @lanes: number of DSI data lanes
 * @dphy_param: D-PHY timing parameters (DPHY_PARAM)
 * @high_low_switch_count: HS/LP switch count
 * @clk_lane_switch_time: clock lane switch time count
 * @lp_byteclk: LP byte clock count
 * @prepare: power the panel up and run its initialization commands, with
 *	the DSI link up in LP mode
 * @enable: turn the display on, once video is streaming
 * @disable: turn the display off and put the panel to sleep, while video
 *	is still streaming
 */
struct mrfld_panel_desc {
	const char *name;
	const struct drm_display_mode *mode;
	unsigned int width_mm;
	unsigned int height_mm;
	unsigned int lanes;
	u32 dphy_param;
	u32 high_low_switch_count;
	u32 clk_lane_switch_time;
	u32 lp_byteclk;
	int (*prepare)(struct mrfld_device *mrfld);
	int (*enable)(struct mrfld_device *mrfld);
	void (*disable)(struct mrfld_device *mrfld);
};

struct mrfld_device {
	struct drm_device drm;
	struct pci_dev *pdev;
	void __iomem *mmio;

	/* GTT: the display controller's view of memory */
	void __iomem *gtt;
	unsigned int gtt_entries;
	unsigned int stolen_entries;
	struct page *scratch_page;
	struct drm_mm gtt_mm;
	struct mutex gtt_lock;		/* protects gtt_mm */

	/* KMS */
	struct drm_plane primary_plane;
	struct drm_crtc crtc;
	struct drm_encoder encoder;
	struct drm_connector connector;
	spinlock_t irq_lock;		/* protects the interrupt enables */
	bool display_on;		/* display island powered, pipe A usable */

	/* DSI */
	struct mipi_dsi_host dsi_host;
	struct mipi_dsi_device *dsi;
	struct mutex dsi_lock;		/* serializes DSI packet traffic */

	/* Panel */
	const struct mrfld_panel_desc *panel;
	struct intel_scu_ipc_dev *scu;
	struct gpio_desc *reset_gpio;
	struct gpio_desc *backlight_gpio;
	struct backlight_device *backlight;
	void __iomem *pwm;
	struct pci_dev *pwm_pdev;
	bool panel_powered_once;
};

static inline struct mrfld_device *to_mrfld(struct drm_device *drm)
{
	return container_of(drm, struct mrfld_device, drm);
}

static inline u32 mrfld_read(struct mrfld_device *mrfld, u32 reg)
{
	return readl(mrfld->mmio + reg);
}

static inline void mrfld_write(struct mrfld_device *mrfld, u32 reg, u32 val)
{
	writel(val, mrfld->mmio + reg);
}

static inline void mrfld_rmw(struct mrfld_device *mrfld, u32 reg, u32 clear, u32 set)
{
	mrfld_write(mrfld, reg, (mrfld_read(mrfld, reg) & ~clear) | set);
}

/* GEM object with a GTT mapping for scanout */
struct mrfld_gem_object {
	struct drm_gem_shmem_object base;
	struct drm_mm_node gtt_node;
	unsigned int gtt_pin_count;	/* protected by gtt_lock */
};

static inline struct mrfld_gem_object *to_mrfld_gem(struct drm_gem_object *obj)
{
	return container_of(to_drm_gem_shmem_obj(obj), struct mrfld_gem_object, base);
}

/* mrfld_gtt.c */
int mrfld_gtt_init(struct mrfld_device *mrfld);
struct drm_gem_object *mrfld_gem_create_object(struct drm_device *drm, size_t size);
int mrfld_gem_dumb_create(struct drm_file *file, struct drm_device *drm,
			  struct drm_mode_create_dumb *args);
int mrfld_gem_gtt_pin(struct mrfld_device *mrfld, struct drm_gem_object *obj, u32 *offset);
void mrfld_gem_gtt_unpin(struct mrfld_device *mrfld, struct drm_gem_object *obj);

/* mrfld_power.c */
int mrfld_power_init(struct mrfld_device *mrfld);
bool mrfld_display_island_is_on(struct mrfld_device *mrfld);
int mrfld_display_island_set(struct mrfld_device *mrfld, bool on);
int mrfld_mio_island_set(struct mrfld_device *mrfld, bool on);
void mrfld_set_display_clock(struct mrfld_device *mrfld);
int mrfld_dsi_pll_enable(struct mrfld_device *mrfld, int lane_rate_khz);
int mrfld_dsi_pll_disable(struct mrfld_device *mrfld);

/* mrfld_dsi.c */
int mrfld_dsi_host_init(struct mrfld_device *mrfld);
void mrfld_dsi_host_fini(struct mrfld_device *mrfld);
void mrfld_dsi_controller_init(struct mrfld_device *mrfld, const struct drm_display_mode *mode);
void mrfld_dsi_link_up(struct mrfld_device *mrfld);
void mrfld_dsi_link_down(struct mrfld_device *mrfld);
void mrfld_dsi_video_on(struct mrfld_device *mrfld);
void mrfld_dsi_video_off(struct mrfld_device *mrfld);

/* mrfld_kms.c */
int mrfld_kms_init(struct mrfld_device *mrfld);
void mrfld_hw_takeover(struct mrfld_device *mrfld);
irqreturn_t mrfld_irq_handler(int irq, void *arg);

/* mrfld_panel.c */
int mrfld_panel_init(struct mrfld_device *mrfld);
int mrfld_panel_power_on(struct mrfld_device *mrfld);
void mrfld_backlight_set(struct mrfld_device *mrfld, bool on);

#endif
