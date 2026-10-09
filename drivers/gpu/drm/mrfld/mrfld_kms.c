// SPDX-License-Identifier: GPL-2.0-only
/*
 * Intel Merrifield/Moorefield display: pipe A, plane A and the DSI panel
 * behind MIPI port A.
 *
 * Turning the CRTC on runs the whole power-up sequence, from the display
 * power island through the DSI PLL to the panel's initialization commands;
 * turning it off runs it backwards, down to the island. Nothing in the
 * display registers is reachable while the island is off.
 */

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/iopoll.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_print.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_vblank.h>

#include "mrfld_drv.h"

/* ---------------------------------------------------------------------- */
/* Plane A                                                                */

static const u32 mrfld_plane_formats[] = {
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_RGB565,
};

static int mrfld_plane_atomic_check(struct drm_plane *plane, struct drm_atomic_commit *state)
{
	struct drm_plane_state *plane_state = drm_atomic_get_new_plane_state(state, plane);
	struct drm_crtc_state *crtc_state = NULL;

	if (plane_state->crtc)
		crtc_state = drm_atomic_get_new_crtc_state(state, plane_state->crtc);

	/* The plane always covers the whole pipe: no scaling, no position */
	return drm_atomic_helper_check_plane_state(plane_state, crtc_state,
						   DRM_PLANE_NO_SCALING, DRM_PLANE_NO_SCALING,
						   false, true);
}

static int mrfld_plane_prepare_fb(struct drm_plane *plane, struct drm_plane_state *plane_state)
{
	struct mrfld_device *mrfld = to_mrfld(plane->dev);
	u32 offset;
	int ret;

	if (!plane_state->fb)
		return 0;

	ret = drm_gem_plane_helper_prepare_fb(plane, plane_state);
	if (ret)
		return ret;

	return mrfld_gem_gtt_pin(mrfld, plane_state->fb->obj[0], &offset);
}

static void mrfld_plane_cleanup_fb(struct drm_plane *plane, struct drm_plane_state *plane_state)
{
	if (plane_state->fb)
		mrfld_gem_gtt_unpin(to_mrfld(plane->dev), plane_state->fb->obj[0]);
}

static void mrfld_plane_atomic_update(struct drm_plane *plane, struct drm_atomic_commit *state)
{
	struct mrfld_device *mrfld = to_mrfld(plane->dev);
	struct drm_plane_state *plane_state = drm_atomic_get_new_plane_state(state, plane);
	struct drm_framebuffer *fb = plane_state->fb;
	struct mrfld_gem_object *bo;
	u32 cntr, linoff;

	if (!fb || !plane_state->visible) {
		mrfld_rmw(mrfld, MRFLD_DSPACNTR, MRFLD_DSPCNTR_ENABLE, 0);
		mrfld_write(mrfld, MRFLD_DSPASURF, mrfld_read(mrfld, MRFLD_DSPASURF));
		return;
	}

	bo = to_mrfld_gem(fb->obj[0]);
	linoff = fb->offsets[0] +
		 (plane_state->src.y1 >> 16) * fb->pitches[0] +
		 (plane_state->src.x1 >> 16) * fb->format->cpp[0];

	/* pipe A, no gamma: the palette does not survive the island */
	cntr = MRFLD_DSPCNTR_ENABLE;
	cntr |= fb->format->format == DRM_FORMAT_RGB565 ?
		MRFLD_DSPCNTR_RGB565 : MRFLD_DSPCNTR_XRGB8888;

	mrfld_write(mrfld, MRFLD_DSPASTRIDE, fb->pitches[0]);
	mrfld_write(mrfld, MRFLD_DSPAPOS, 0);
	mrfld_write(mrfld, MRFLD_DSPASIZE,
		    (drm_rect_height(&plane_state->dst) - 1) << 16 |
		    (drm_rect_width(&plane_state->dst) - 1));
	mrfld_write(mrfld, MRFLD_DSPALINOFF, linoff);
	mrfld_write(mrfld, MRFLD_DSPACNTR, cntr);
	/* the surface address write latches the update at the next vblank */
	mrfld_write(mrfld, MRFLD_DSPASURF, bo->gtt_node.start << PAGE_SHIFT);
}

static const struct drm_plane_helper_funcs mrfld_plane_helper_funcs = {
	.prepare_fb = mrfld_plane_prepare_fb,
	.cleanup_fb = mrfld_plane_cleanup_fb,
	.atomic_check = mrfld_plane_atomic_check,
	.atomic_update = mrfld_plane_atomic_update,
};

static const struct drm_plane_funcs mrfld_plane_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.destroy = drm_plane_cleanup,
	.reset = drm_atomic_helper_plane_reset,
	.atomic_duplicate_state = drm_atomic_helper_plane_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_plane_destroy_state,
};

/* ---------------------------------------------------------------------- */
/* Pipe A                                                                 */

/* Display controller defaults for Anniedale: FIFOs, clock gating */
static void mrfld_dc_setup(struct mrfld_device *mrfld)
{
	mrfld_write(mrfld, MRFLD_DSPCLK_GATE_D, 0);
	mrfld_write(mrfld, MRFLD_RAMCLK_GATE_D, 0xc0000 | BIT(11));
	mrfld_write(mrfld, MRFLD_DSPARB2, 0x000a0200);
	mrfld_write(mrfld, MRFLD_DSPARB, 0x18040080);
	mrfld_write(mrfld, MRFLD_DSPFW1, 0x0f0f3f3f);
	mrfld_write(mrfld, MRFLD_DSPFW2, 0x5f2f0f3f);
	mrfld_write(mrfld, MRFLD_DSPFW3, 0);
	mrfld_write(mrfld, MRFLD_DSPFW4, 0x07071f1f);
	mrfld_write(mrfld, MRFLD_DSPFW5, 0x2f17071f);
	mrfld_write(mrfld, MRFLD_DSPFW6, 0x00001f3f);
	mrfld_write(mrfld, MRFLD_DSPFW7, 0x1f3f1f3f);
	mrfld_write(mrfld, MRFLD_DSPSRCTRL, 0x00080100);
	mrfld_write(mrfld, MRFLD_DSPCHICKENBIT, 0x20);
	mrfld_write(mrfld, MRFLD_FBDC_CHICKEN, 0x0c0c0c0c);
	mrfld_write(mrfld, MRFLD_CURACNTR, 0);
	/*
	 * Fixed arbitration: display TLB requests are otherwise starved by
	 * plane fetches, and the plane reads through stale translations.
	 */
	mrfld_rmw(mrfld, MRFLD_GCI_CTRL, 0, MRFLD_GCI_FIXED_ARB);
}

static void mrfld_set_timings(struct mrfld_device *mrfld, const struct drm_display_mode *mode)
{
	mrfld_write(mrfld, MRFLD_HTOTAL_A, (mode->hdisplay - 1) | (mode->htotal - 1) << 16);
	mrfld_write(mrfld, MRFLD_HBLANK_A, (mode->hdisplay - 1) | (mode->htotal - 1) << 16);
	mrfld_write(mrfld, MRFLD_HSYNC_A, (mode->hsync_start - 1) | (mode->hsync_end - 1) << 16);
	mrfld_write(mrfld, MRFLD_VTOTAL_A, (mode->vdisplay - 1) | (mode->vtotal - 1) << 16);
	mrfld_write(mrfld, MRFLD_VBLANK_A, (mode->vdisplay - 1) | (mode->vtotal - 1) << 16);
	mrfld_write(mrfld, MRFLD_VSYNC_A, (mode->vsync_start - 1) | (mode->vsync_end - 1) << 16);
	mrfld_write(mrfld, MRFLD_PIPEASRC, (mode->hdisplay - 1) << 16 | (mode->vdisplay - 1));
	mrfld_write(mrfld, MRFLD_VGACNTRL, MRFLD_VGA_DISABLE);
}

static int mrfld_pipe_on(struct mrfld_device *mrfld, const struct drm_display_mode *mode)
{
	const struct mrfld_panel_desc *panel = mrfld->panel;
	u32 val;
	int ret;

	ret = mrfld_display_island_set(mrfld, true);
	if (ret)
		return ret;
	mrfld->display_on = true;

	mrfld_set_display_clock(mrfld);
	ret = mrfld_dsi_pll_enable(mrfld, mode->clock * 24 / panel->lanes);
	if (ret)
		return ret;
	ret = mrfld_mio_island_set(mrfld, true);
	if (ret)
		return ret;

	ret = readl_poll_timeout(mrfld->mmio + MRFLD_PIPEACONF, val,
				 val & MRFLD_PIPECONF_DSIPLL_LOCK, 150, 3000000);
	if (ret) {
		drm_err(&mrfld->drm, "pipe A sees no DSI PLL lock\n");
		return ret;
	}

	mrfld_dc_setup(mrfld);
	mrfld_dsi_controller_init(mrfld, mode);
	mrfld_set_timings(mrfld, mode);

	mrfld_dsi_link_up(mrfld);
	ret = mrfld_panel_power_on(mrfld);
	if (ret)
		drm_err(&mrfld->drm, "panel initialization failed: %d\n", ret);
	mrfld_dsi_video_on(mrfld);

	mrfld_write(mrfld, MRFLD_DDL1, 0x86868686);
	mrfld_write(mrfld, MRFLD_DDL2, 0x86868686);
	mrfld_write(mrfld, MRFLD_DDL3, 0x86);
	mrfld_write(mrfld, MRFLD_DDL4, 0x8686);
	mrfld_write(mrfld, MRFLD_DSPARB2, 0x00090180);
	mrfld_write(mrfld, MRFLD_DSPARB, 0x0c0300c0);

	/* frame start on the third HBLANK after the start of VBLANK */
	val = mrfld_read(mrfld, MRFLD_PIPEACONF);
	val &= ~(MRFLD_PIPECONF_FRAME_START | MRFLD_PIPECONF_PLANE_OFF | MRFLD_PIPECONF_CURSOR_OFF);
	val |= FIELD_PREP(MRFLD_PIPECONF_FRAME_START, 2) | MRFLD_PIPECONF_ENABLE;
	mrfld_write(mrfld, MRFLD_PIPEACONF, val);

	ret = readl_poll_timeout(mrfld->mmio + MRFLD_PIPEACONF, val,
				 val & MRFLD_PIPECONF_STATE, 3, 30000);
	if (ret)
		drm_err(&mrfld->drm, "pipe A did not start\n");

	return ret;
}

static void mrfld_pipe_off(struct mrfld_device *mrfld)
{
	unsigned long flags;
	u32 val;

	/* nothing may interrupt a pipe that is down, island included */
	spin_lock_irqsave(&mrfld->irq_lock, flags);
	mrfld_rmw(mrfld, MRFLD_PIPEASTAT, MRFLD_PIPE_VBLANK_ENABLE, 0);
	mrfld_write(mrfld, MRFLD_INT_ENABLE, 0);
	mrfld_write(mrfld, MRFLD_INT_MASK, ~0u);
	spin_unlock_irqrestore(&mrfld->irq_lock, flags);

	mrfld_rmw(mrfld, MRFLD_DSPACNTR, MRFLD_DSPCNTR_ENABLE, 0);
	mrfld_write(mrfld, MRFLD_DSPASURF, mrfld_read(mrfld, MRFLD_DSPASURF));

	mrfld_rmw(mrfld, MRFLD_PIPEACONF, 0, MRFLD_PIPECONF_PLANE_OFF | MRFLD_PIPECONF_CURSOR_OFF);
	mrfld_rmw(mrfld, MRFLD_PIPEACONF, MRFLD_PIPECONF_ENABLE, 0);
	if (mrfld_read(mrfld, MRFLD_MIPI) & MRFLD_MIPI_PORT_EN &&
	    readl_poll_timeout(mrfld->mmio + MRFLD_PIPEACONF, val,
			       !(val & MRFLD_PIPECONF_STATE), 5, 500000))
		drm_warn(&mrfld->drm, "pipe A did not stop\n");

	mrfld_dsi_video_off(mrfld);
	mrfld->panel->disable(mrfld);
	mrfld_dsi_link_down(mrfld);

	mrfld_dsi_pll_disable(mrfld);
	mrfld_mio_island_set(mrfld, false);
	mrfld->display_on = false;
	mrfld_display_island_set(mrfld, false);
}

/*
 * Firmware hands over with the panel lit. Turn it all off, so the first
 * modeset starts from a known state and runs the full power-up sequence.
 */
void mrfld_hw_takeover(struct mrfld_device *mrfld)
{
	if (!mrfld_display_island_is_on(mrfld))
		return;

	mrfld->display_on = true;
	if (!(mrfld_read(mrfld, MRFLD_PIPEACONF) & MRFLD_PIPECONF_ENABLE))
		return;

	drm_dbg_kms(&mrfld->drm, "taking over the firmware's display\n");
	mrfld_backlight_set(mrfld, false);
	mrfld_pipe_off(mrfld);
}

static int mrfld_crtc_atomic_check(struct drm_crtc *crtc, struct drm_atomic_commit *state)
{
	struct drm_crtc_state *crtc_state = drm_atomic_get_new_crtc_state(state, crtc);

	if (!crtc_state->enable)
		return 0;

	return drm_atomic_helper_check_crtc_primary_plane(crtc_state);
}

static enum drm_mode_status mrfld_crtc_mode_valid(struct drm_crtc *crtc,
						  const struct drm_display_mode *mode)
{
	struct mrfld_device *mrfld = to_mrfld(crtc->dev);

	return drm_mode_equal(mode, mrfld->panel->mode) ? MODE_OK : MODE_BAD;
}

static void mrfld_crtc_atomic_enable(struct drm_crtc *crtc, struct drm_atomic_commit *state)
{
	struct mrfld_device *mrfld = to_mrfld(crtc->dev);
	struct drm_crtc_state *crtc_state = drm_atomic_get_new_crtc_state(state, crtc);
	int ret;

	ret = mrfld_pipe_on(mrfld, &crtc_state->adjusted_mode);
	if (ret)
		drm_err(&mrfld->drm, "display power up failed: %d\n", ret);

	drm_crtc_vblank_on(crtc);

	ret = mrfld->panel->enable(mrfld);
	if (ret)
		drm_err(&mrfld->drm, "panel did not turn on: %d\n", ret);
	mrfld_backlight_set(mrfld, true);
}

static void mrfld_crtc_atomic_disable(struct drm_crtc *crtc, struct drm_atomic_commit *state)
{
	struct mrfld_device *mrfld = to_mrfld(crtc->dev);

	mrfld_backlight_set(mrfld, false);
	drm_crtc_vblank_off(crtc);
	mrfld_pipe_off(mrfld);

	if (crtc->state->event && !crtc->state->active) {
		spin_lock_irq(&crtc->dev->event_lock);
		drm_crtc_send_vblank_event(crtc, crtc->state->event);
		crtc->state->event = NULL;
		spin_unlock_irq(&crtc->dev->event_lock);
	}
}

static void mrfld_crtc_atomic_flush(struct drm_crtc *crtc, struct drm_atomic_commit *state)
{
	struct drm_pending_vblank_event *event = crtc->state->event;

	if (!event)
		return;

	crtc->state->event = NULL;
	spin_lock_irq(&crtc->dev->event_lock);
	if (crtc->state->active && drm_crtc_vblank_get(crtc) == 0)
		drm_crtc_arm_vblank_event(crtc, event);
	else
		drm_crtc_send_vblank_event(crtc, event);
	spin_unlock_irq(&crtc->dev->event_lock);
}

static const struct drm_crtc_helper_funcs mrfld_crtc_helper_funcs = {
	.mode_valid = mrfld_crtc_mode_valid,
	.atomic_check = mrfld_crtc_atomic_check,
	.atomic_flush = mrfld_crtc_atomic_flush,
	.atomic_enable = mrfld_crtc_atomic_enable,
	.atomic_disable = mrfld_crtc_atomic_disable,
};

static int mrfld_enable_vblank(struct drm_crtc *crtc)
{
	struct mrfld_device *mrfld = to_mrfld(crtc->dev);
	unsigned long flags;

	spin_lock_irqsave(&mrfld->irq_lock, flags);
	if (mrfld->display_on) {
		mrfld_write(mrfld, MRFLD_PIPEASTAT,
			    mrfld_read(mrfld, MRFLD_PIPEASTAT) |
			    MRFLD_PIPE_VBLANK_ENABLE | MRFLD_PIPE_VBLANK_STATUS);
		mrfld_write(mrfld, MRFLD_INT_MASK, ~(u32)MRFLD_INT_PIPEA_EVENT);
		mrfld_write(mrfld, MRFLD_INT_ENABLE, MRFLD_INT_PIPEA_EVENT);
	}
	spin_unlock_irqrestore(&mrfld->irq_lock, flags);

	return mrfld->display_on ? 0 : -EINVAL;
}

static void mrfld_disable_vblank(struct drm_crtc *crtc)
{
	struct mrfld_device *mrfld = to_mrfld(crtc->dev);
	unsigned long flags;

	spin_lock_irqsave(&mrfld->irq_lock, flags);
	if (mrfld->display_on) {
		mrfld_rmw(mrfld, MRFLD_PIPEASTAT, MRFLD_PIPE_VBLANK_ENABLE, 0);
		mrfld_write(mrfld, MRFLD_INT_ENABLE, 0);
		mrfld_write(mrfld, MRFLD_INT_MASK, ~0u);
	}
	spin_unlock_irqrestore(&mrfld->irq_lock, flags);
}

static const struct drm_crtc_funcs mrfld_crtc_funcs = {
	.reset = drm_atomic_helper_crtc_reset,
	.destroy = drm_crtc_cleanup,
	.set_config = drm_atomic_helper_set_config,
	.page_flip = drm_atomic_helper_page_flip,
	.atomic_duplicate_state = drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_crtc_destroy_state,
	.enable_vblank = mrfld_enable_vblank,
	.disable_vblank = mrfld_disable_vblank,
};

irqreturn_t mrfld_irq_handler(int irq, void *arg)
{
	struct mrfld_device *mrfld = arg;
	u32 identity, pipestat;

	spin_lock(&mrfld->irq_lock);
	if (!mrfld->display_on) {
		spin_unlock(&mrfld->irq_lock);
		return IRQ_NONE;
	}

	identity = mrfld_read(mrfld, MRFLD_INT_IDENTITY);
	if (!(identity & MRFLD_INT_PIPEA_EVENT)) {
		spin_unlock(&mrfld->irq_lock);
		return IRQ_NONE;
	}

	/* DSI errors, FIFO underruns included, are only acknowledged */
	mrfld_write(mrfld, MRFLD_DSI_INTR_STAT, mrfld_read(mrfld, MRFLD_DSI_INTR_STAT));

	pipestat = mrfld_read(mrfld, MRFLD_PIPEASTAT);
	mrfld_write(mrfld, MRFLD_PIPEASTAT, pipestat);
	mrfld_write(mrfld, MRFLD_INT_IDENTITY, identity);
	spin_unlock(&mrfld->irq_lock);

	if (pipestat & MRFLD_PIPE_VBLANK_STATUS)
		drm_crtc_handle_vblank(&mrfld->crtc);

	return IRQ_HANDLED;
}

/* ---------------------------------------------------------------------- */
/* DSI connector                                                          */

static int mrfld_connector_get_modes(struct drm_connector *connector)
{
	struct mrfld_device *mrfld = to_mrfld(connector->dev);

	return drm_connector_helper_get_modes_fixed(connector, mrfld->panel->mode);
}

static const struct drm_connector_helper_funcs mrfld_connector_helper_funcs = {
	.get_modes = mrfld_connector_get_modes,
};

static const struct drm_connector_funcs mrfld_connector_funcs = {
	.fill_modes = drm_helper_probe_single_connector_modes,
	.reset = drm_atomic_helper_connector_reset,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static const struct drm_mode_config_funcs mrfld_mode_config_funcs = {
	.fb_create = drm_gem_fb_create,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

/* Planes are only touched while their CRTC is on: the island may be off */
static const struct drm_mode_config_helper_funcs mrfld_mode_config_helper_funcs = {
	.atomic_commit_tail = drm_atomic_helper_commit_tail_rpm,
};

int mrfld_kms_init(struct mrfld_device *mrfld)
{
	struct drm_device *drm = &mrfld->drm;
	const struct drm_display_mode *mode = mrfld->panel->mode;
	int ret;

	ret = drmm_mode_config_init(drm);
	if (ret)
		return ret;

	drm->mode_config.min_width = mode->hdisplay;
	drm->mode_config.max_width = mode->hdisplay;
	drm->mode_config.min_height = mode->vdisplay;
	drm->mode_config.max_height = mode->vdisplay;
	drm->mode_config.preferred_depth = 24;
	drm->mode_config.funcs = &mrfld_mode_config_funcs;
	drm->mode_config.helper_private = &mrfld_mode_config_helper_funcs;

	ret = drm_universal_plane_init(drm, &mrfld->primary_plane, 0, &mrfld_plane_funcs,
				       mrfld_plane_formats, ARRAY_SIZE(mrfld_plane_formats),
				       NULL, DRM_PLANE_TYPE_PRIMARY, NULL);
	if (ret)
		return ret;
	drm_plane_helper_add(&mrfld->primary_plane, &mrfld_plane_helper_funcs);

	ret = drm_crtc_init_with_planes(drm, &mrfld->crtc, &mrfld->primary_plane, NULL,
					&mrfld_crtc_funcs, NULL);
	if (ret)
		return ret;
	drm_crtc_helper_add(&mrfld->crtc, &mrfld_crtc_helper_funcs);

	ret = drmm_encoder_init(drm, &mrfld->encoder, NULL, DRM_MODE_ENCODER_DSI, NULL);
	if (ret)
		return ret;
	mrfld->encoder.possible_crtcs = drm_crtc_mask(&mrfld->crtc);

	ret = drmm_connector_init(drm, &mrfld->connector, &mrfld_connector_funcs,
				  DRM_MODE_CONNECTOR_DSI, NULL);
	if (ret)
		return ret;
	drm_connector_helper_add(&mrfld->connector, &mrfld_connector_helper_funcs);
	mrfld->connector.display_info.width_mm = mrfld->panel->width_mm;
	mrfld->connector.display_info.height_mm = mrfld->panel->height_mm;
	ret = drm_connector_attach_encoder(&mrfld->connector, &mrfld->encoder);
	if (ret)
		return ret;

	/* the hardware frame counter reads 0 on Moorefield: count in software */
	ret = drm_vblank_init(drm, 1);
	if (ret)
		return ret;
	drm->max_vblank_count = 0;

	drm_mode_config_reset(drm);
	return 0;
}
