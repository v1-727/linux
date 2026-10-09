// SPDX-License-Identifier: GPL-2.0-only
/*
 * Intel Merrifield/Moorefield MIPI DSI host, port A, video mode
 *
 * Commands go out through the generic packet FIFOs: in LP mode before the
 * video stream starts, in either mode once it runs. Each packet is written
 * as its payload (long packets) followed by a control word that holds the
 * packet header.
 */

#include <linux/delay.h>
#include <linux/iopoll.h>

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_print.h>

#include "mrfld_drv.h"

#define MRFLD_DSI_FIFO_TIMEOUT_US	30000
#define MRFLD_DSI_READ_TIMEOUT_US	30000

static inline struct mrfld_device *host_to_mrfld(struct mipi_dsi_host *host)
{
	return container_of(host, struct mrfld_device, dsi_host);
}

static int mrfld_dsi_wait_fifo(struct mrfld_device *mrfld, u32 empty_mask)
{
	u32 stat;
	int ret;

	ret = readl_poll_timeout(mrfld->mmio + MRFLD_DSI_GEN_FIFO_STAT, stat,
				 (stat & empty_mask) == empty_mask, 3,
				 MRFLD_DSI_FIFO_TIMEOUT_US);
	if (ret)
		drm_err(&mrfld->drm, "DSI FIFOs not empty: %#x\n", stat);
	return ret;
}

static void mrfld_dsi_write_fifo(struct mrfld_device *mrfld, u32 reg,
				 const u8 *data, size_t len)
{
	while (len) {
		size_t n = min(len, 4);
		u32 word = 0;
		size_t i;

		for (i = 0; i < n; i++)
			word |= data[i] << (8 * i);
		mrfld_write(mrfld, reg, word);
		data += n;
		len -= n;
	}
}

static ssize_t mrfld_dsi_read_response(struct mrfld_device *mrfld, u32 data_reg,
				       u8 *buf, size_t len)
{
	size_t i;
	u32 stat;
	int ret;

	ret = readl_poll_timeout(mrfld->mmio + MRFLD_DSI_INTR_STAT, stat,
				 stat & MRFLD_DSI_RX_DATA_VALID, 3,
				 MRFLD_DSI_READ_TIMEOUT_US);
	if (ret)
		return ret;
	mrfld_write(mrfld, MRFLD_DSI_INTR_STAT, MRFLD_DSI_RX_DATA_VALID);

	for (i = 0; i < len; i += 4) {
		u32 word = mrfld_read(mrfld, data_reg);
		size_t j;

		for (j = 0; j < 4 && i + j < len; j++)
			buf[i + j] = word >> (8 * j);
	}

	return len;
}

static ssize_t mrfld_dsi_transfer(struct mipi_dsi_host *host,
				  const struct mipi_dsi_msg *msg)
{
	struct mrfld_device *mrfld = host_to_mrfld(host);
	bool lp = msg->flags & MIPI_DSI_MSG_USE_LPM;
	u32 ctrl_reg = lp ? MRFLD_DSI_LP_GEN_CTRL : MRFLD_DSI_HS_GEN_CTRL;
	u32 data_reg = lp ? MRFLD_DSI_LP_GEN_DATA : MRFLD_DSI_HS_GEN_DATA;
	u32 empty = MRFLD_DSI_FIFO_DBI_EMPTY |
		    (lp ? MRFLD_DSI_FIFO_LP_DATA_EMPTY | MRFLD_DSI_FIFO_LP_CTRL_EMPTY :
			  MRFLD_DSI_FIFO_HS_DATA_EMPTY | MRFLD_DSI_FIFO_HS_CTRL_EMPTY);
	struct mipi_dsi_packet packet;
	ssize_t ret;

	ret = mipi_dsi_create_packet(&packet, msg);
	if (ret)
		return ret;

	if (msg->rx_len > 64)
		return -EINVAL;	/* generic read FIFO size */

	guard(mutex)(&mrfld->dsi_lock);

	ret = mrfld_dsi_wait_fifo(mrfld, empty);
	if (ret)
		return ret;

	if (msg->rx_len) {
		mrfld_write(mrfld, MRFLD_DSI_MAX_RETURN_PKT_SIZE, msg->rx_len);
		mrfld_write(mrfld, MRFLD_DSI_INTR_STAT, MRFLD_DSI_RX_DATA_VALID);
	}

	if (mipi_dsi_packet_format_is_long(msg->type))
		mrfld_dsi_write_fifo(mrfld, data_reg, packet.payload, packet.payload_length);

	/* data type and channel, then data bytes or word count */
	mrfld_write(mrfld, ctrl_reg,
		    packet.header[0] |
		    packet.header[1] << MRFLD_DSI_GEN_DATA0_SHIFT |
		    packet.header[2] << MRFLD_DSI_GEN_DATA1_SHIFT);

	if (msg->rx_len)
		return mrfld_dsi_read_response(mrfld, data_reg, msg->rx_buf, msg->rx_len);

	ret = mrfld_dsi_wait_fifo(mrfld, empty);
	return ret ? ret : packet.size;
}

static int mrfld_dsi_attach(struct mipi_dsi_host *host, struct mipi_dsi_device *dsi)
{
	return 0;
}

static int mrfld_dsi_detach(struct mipi_dsi_host *host, struct mipi_dsi_device *dsi)
{
	return 0;
}

static const struct mipi_dsi_host_ops mrfld_dsi_host_ops = {
	.attach = mrfld_dsi_attach,
	.detach = mrfld_dsi_detach,
	.transfer = mrfld_dsi_transfer,
};

int mrfld_dsi_host_init(struct mrfld_device *mrfld)
{
	const struct mrfld_panel_desc *panel = mrfld->panel;
	struct mipi_dsi_device_info info = { .channel = 0 };
	int ret;

	mutex_init(&mrfld->dsi_lock);

	mrfld->dsi_host.dev = &mrfld->pdev->dev;
	mrfld->dsi_host.ops = &mrfld_dsi_host_ops;
	ret = mipi_dsi_host_register(&mrfld->dsi_host);
	if (ret)
		return ret;

	/* No firmware node describes the panel: register it by name */
	strscpy(info.type, panel->name, sizeof(info.type));
	mrfld->dsi = mipi_dsi_device_register_full(&mrfld->dsi_host, &info);
	if (IS_ERR(mrfld->dsi)) {
		mipi_dsi_host_unregister(&mrfld->dsi_host);
		return PTR_ERR(mrfld->dsi);
	}

	mrfld->dsi->lanes = panel->lanes;
	mrfld->dsi->format = MIPI_DSI_FMT_RGB888;
	mrfld->dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_LPM;

	return 0;
}

void mrfld_dsi_host_fini(struct mrfld_device *mrfld)
{
	mipi_dsi_device_unregister(mrfld->dsi);
	mipi_dsi_host_unregister(&mrfld->dsi_host);
}

/* Horizontal DPI timings are counted in byte clocks of one lane */
static u32 mrfld_dsi_byte_clocks(const struct mrfld_panel_desc *panel, int pixels)
{
	return pixels * 24 / (panel->lanes * 8);
}

void mrfld_dsi_controller_init(struct mrfld_device *mrfld, const struct drm_display_mode *mode)
{
	const struct mrfld_panel_desc *panel = mrfld->panel;

	mrfld_write(mrfld, MRFLD_DSI_DPHY_PARAM, panel->dphy_param);
	mrfld_write(mrfld, MRFLD_DSI_CONTROL, 0x18);
	mrfld_write(mrfld, MRFLD_DSI_INTR_EN, 0xffffffff);
	mrfld_write(mrfld, MRFLD_DSI_HS_TX_TIMEOUT, 0xffffff);
	mrfld_write(mrfld, MRFLD_DSI_LP_RX_TIMEOUT, 0xffffff);
	mrfld_write(mrfld, MRFLD_DSI_TURN_AROUND_TIMEOUT, 0xffff);
	mrfld_write(mrfld, MRFLD_DSI_DEVICE_RESET_TIMER, 0xffff);
	mrfld_write(mrfld, MRFLD_DSI_HIGH_LOW_SWITCH_COUNT, panel->high_low_switch_count);
	mrfld_write(mrfld, MRFLD_DSI_INIT_COUNT, 0x7d0);
	mrfld_rmw(mrfld, MRFLD_DSI_EOT_DISABLE, MRFLD_DSI_EOT_DISABLE_MASK, 0x3);
	mrfld_write(mrfld, MRFLD_DSI_LP_BYTECLK, panel->lp_byteclk);
	mrfld_write(mrfld, MRFLD_DSI_CLK_LANE_SWITCH_TIME, panel->clk_lane_switch_time);
	mrfld_write(mrfld, MRFLD_DSI_VIDEO_MODE_FORMAT, 0xf);
	mrfld_write(mrfld, MRFLD_DSI_FUNC_PRG, MRFLD_DSI_FMT_RGB888 | panel->lanes);

	mrfld_write(mrfld, MRFLD_DSI_DPI_RESOLUTION, mode->vdisplay << 16 | mode->hdisplay);
	mrfld_write(mrfld, MRFLD_DSI_HSYNC_COUNT,
		    mrfld_dsi_byte_clocks(panel, mode->hsync_end - mode->hsync_start));
	mrfld_write(mrfld, MRFLD_DSI_HBP_COUNT,
		    mrfld_dsi_byte_clocks(panel, mode->htotal - mode->hsync_end));
	mrfld_write(mrfld, MRFLD_DSI_HFP_COUNT,
		    mrfld_dsi_byte_clocks(panel, mode->hsync_start - mode->hdisplay));
	mrfld_write(mrfld, MRFLD_DSI_HACTIVE_COUNT, mrfld_dsi_byte_clocks(panel, mode->hdisplay));
	mrfld_write(mrfld, MRFLD_DSI_VSYNC_COUNT, mode->vsync_end - mode->vsync_start);
	mrfld_write(mrfld, MRFLD_DSI_VBP_COUNT, mode->vtotal - mode->vsync_end);
	mrfld_write(mrfld, MRFLD_DSI_VFP_COUNT, mode->vsync_start - mode->vdisplay);
}

#define MRFLD_MIPI_PORT_BASE \
	(MRFLD_MIPI_PORT_EN | MRFLD_MIPI_PASS_TO_AFE | MRFLD_MIPI_BANDGAP_CHICKEN)

/* Bring the link out of ULPS and the controller up, ready for LP commands */
void mrfld_dsi_link_up(struct mrfld_device *mrfld)
{
	u32 ready = mrfld_read(mrfld, MRFLD_DSI_DEVICE_READY);

	ready = (ready & ~MRFLD_DSI_ULPS_MASK) | MRFLD_DSI_ULPS_ENTER | MRFLD_DSI_READY;
	mrfld_write(mrfld, MRFLD_DSI_DEVICE_READY, ready);
	usleep_range(1000, 1100);

	mrfld_rmw(mrfld, MRFLD_MIPI, 0, MRFLD_MIPI_PASS_TO_AFE);

	ready = (ready & ~MRFLD_DSI_ULPS_MASK) | MRFLD_DSI_ULPS_EXIT | MRFLD_DSI_READY;
	mrfld_write(mrfld, MRFLD_DSI_DEVICE_READY, ready);
	usleep_range(1000, 1100);

	ready &= ~MRFLD_DSI_ULPS_MASK;
	mrfld_write(mrfld, MRFLD_DSI_DEVICE_READY, ready);
	usleep_range(1000, 1100);

	mrfld_write(mrfld, MRFLD_MIPI, MRFLD_MIPI_PORT_BASE);
}

/* Stop the port and park the link in ULPS */
void mrfld_dsi_link_down(struct mrfld_device *mrfld)
{
	u32 ready;

	mrfld_rmw(mrfld, MRFLD_MIPI, MRFLD_MIPI_PORT_EN, 0);

	mrfld_dsi_wait_fifo(mrfld, MRFLD_DSI_FIFO_HS_DATA_EMPTY | MRFLD_DSI_FIFO_LP_DATA_EMPTY |
			    MRFLD_DSI_FIFO_HS_CTRL_EMPTY | MRFLD_DSI_FIFO_LP_CTRL_EMPTY |
			    MRFLD_DSI_FIFO_DBI_EMPTY | MRFLD_DSI_FIFO_DPI_EMPTY);

	ready = mrfld_read(mrfld, MRFLD_DSI_DEVICE_READY);
	ready = (ready & ~MRFLD_DSI_ULPS_MASK) | MRFLD_DSI_ULPS_ENTER | MRFLD_DSI_READY;
	mrfld_write(mrfld, MRFLD_DSI_DEVICE_READY, ready);
	usleep_range(1000, 1100);

	mrfld_rmw(mrfld, MRFLD_MIPI, MRFLD_MIPI_PASS_TO_AFE, 0);
}

/* Start streaming the pipe's output in HS mode */
void mrfld_dsi_video_on(struct mrfld_device *mrfld)
{
	mrfld_write(mrfld, MRFLD_MIPI, MRFLD_MIPI_PORT_BASE);
	mrfld_write(mrfld, MRFLD_DSI_DPI_CONTROL, MRFLD_DSI_DPI_TURN_ON);
}

void mrfld_dsi_video_off(struct mrfld_device *mrfld)
{
	mrfld_dsi_wait_fifo(mrfld, MRFLD_DSI_FIFO_DPI_EMPTY);
}
