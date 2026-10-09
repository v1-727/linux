// SPDX-License-Identifier: GPL-2.0-only
/*
 * Intel Merrifield/Moorefield display power islands and DSI PLL
 *
 * Both are reached over the IOSF sideband: the display and MIPI I/O power
 * islands through the P-Unit, the display clock and the DSI PLL through the
 * clock control unit (CCK).
 */

#include <linux/delay.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/string_choices.h>

#include <asm/iosf_mbi.h>

#include <drm/drm_print.h>

#include "mrfld_drv.h"

/* P-Unit: power island control, two bits per island, state mirrored at 24 */
#define PUNIT_PORT			0x04
#define PUNIT_DSP_SS_PM			0x36
#define   DSP_SS_DPA			GENMASK(1, 0)	/* pipe A + MIPI A */
#define PUNIT_MIO_SS_PM			0x3b
#define   MIO_SS			GENMASK(1, 0)
#define PUNIT_SSS_SHIFT			24
#define PUNIT_ISLAND_ON			0x0
#define PUNIT_ISLAND_OFF		0x3

/* CCK: display clock and DSI PLL */
#define CCK_PORT			0x14
#define CCK_DSI_PLL_CTRL		0x48
#define   DSI_PLL_VCO_EN		BIT(31)
#define   DSI_PLL_LDO_EN		BIT(30)
#define   DSI_PLL_P1_MASK		GENMASK(25, 17)
#define   DSI_PLL_P1(p1)		(BIT((p1) - 2) << 17)
#define   DSI_PLL_CCK_SELECT		BIT(11)
#define   DSI_PLL_MUX_CCK_DSI0		BIT(10)
#define   DSI_PLL_MUX_CCK_DSI1		BIT(9)
#define   DSI_PLL_CLK_EN_DSI0		BIT(8)
#define   DSI_PLL_CLK_EN_MASK		GENMASK(8, 5)
#define   DSI_PLL_LOCK			BIT(0)
#define CCK_DSI_PLL_DIV			0x4c
#define CCK_FREQ_CNTRL5			0x68
#define   CK_ESC_GATE_EN		BIT(10)
#define   CK_DP1X_GATE_EN		BIT(9)
#define   DISPLAY_FREQ_EN		BIT(7)
#define   DISPLAY_FREQ_333MHZ		2

/* IOSF message bus through the host bridge, when iosf_mbi does not own it */
#define MBI_MCR				0xd0
#define MBI_MDR				0xd4
#define MBI_MCRX			0xd8
#define MBI_OP_READ			0x10
#define MBI_OP_WRITE			0x11
#define MBI_BYTE_ENABLES		0xf0

static DEFINE_MUTEX(mbi_lock);
static struct pci_dev *mbi_bridge;

static u32 mbi_cmd(u8 opcode, u8 port, u32 reg)
{
	return (opcode << 24) | (port << 16) | ((reg & 0xff) << 8) | MBI_BYTE_ENABLES;
}

static u32 mbi_read(u8 port, u32 reg)
{
	u32 val = 0;

	if (iosf_mbi_available()) {
		iosf_mbi_read(port, MBI_OP_READ, reg, &val);
		return val;
	}

	guard(mutex)(&mbi_lock);
	pci_write_config_dword(mbi_bridge, MBI_MCRX, reg & 0xffffff00);
	pci_write_config_dword(mbi_bridge, MBI_MCR, mbi_cmd(MBI_OP_READ, port, reg));
	pci_read_config_dword(mbi_bridge, MBI_MDR, &val);
	return val;
}

static void mbi_write(u8 port, u32 reg, u32 val)
{
	if (iosf_mbi_available()) {
		iosf_mbi_write(port, MBI_OP_WRITE, reg, val);
		return;
	}

	guard(mutex)(&mbi_lock);
	pci_write_config_dword(mbi_bridge, MBI_MDR, val);
	pci_write_config_dword(mbi_bridge, MBI_MCRX, reg & 0xffffff00);
	pci_write_config_dword(mbi_bridge, MBI_MCR, mbi_cmd(MBI_OP_WRITE, port, reg));
}

int mrfld_power_init(struct mrfld_device *mrfld)
{
	if (iosf_mbi_available())
		return 0;

	/*
	 * iosf_mbi does not know this SoC's host bridge: use its message bus
	 * registers directly, which is safe as nothing else does then.
	 */
	mbi_bridge = pci_get_domain_bus_and_slot(0, 0, PCI_DEVFN(0, 0));
	if (!mbi_bridge) {
		drm_err(&mrfld->drm, "no host bridge for the IOSF message bus\n");
		return -ENODEV;
	}
	drm_dbg_driver(&mrfld->drm, "IOSF message bus via host bridge %04x:%04x\n",
		       mbi_bridge->vendor, mbi_bridge->device);
	return 0;
}

static int punit_island_set(struct mrfld_device *mrfld, u32 reg, u32 mask, bool on)
{
	u32 want = (on ? PUNIT_ISLAND_ON : PUNIT_ISLAND_OFF) * (mask & -mask);
	u32 val;
	int i;

	val = mbi_read(PUNIT_PORT, reg);
	if ((((val >> PUNIT_SSS_SHIFT) ^ want) & mask) == 0)
		return 0;

	mbi_write(PUNIT_PORT, reg, (val & ~mask) | want);

	for (i = 0; i < 100; i++) {
		val = mbi_read(PUNIT_PORT, reg);
		if ((((val >> PUNIT_SSS_SHIFT) ^ want) & mask) == 0)
			return 0;
		usleep_range(10, 20);
	}

	drm_err(&mrfld->drm, "P-Unit island %#x did not turn %s (%#x)\n",
		reg, str_on_off(on), val);
	return -ETIMEDOUT;
}

bool mrfld_display_island_is_on(struct mrfld_device *mrfld)
{
	u32 val = mbi_read(PUNIT_PORT, PUNIT_DSP_SS_PM);

	return ((val >> PUNIT_SSS_SHIFT) & DSP_SS_DPA) == PUNIT_ISLAND_ON;
}

int mrfld_display_island_set(struct mrfld_device *mrfld, bool on)
{
	int ret;

	ret = punit_island_set(mrfld, PUNIT_DSP_SS_PM, DSP_SS_DPA, on);
	if (ret || !on)
		return ret;

	/* HSD 4582616: czclk stays gated after frame start, causing underruns */
	mrfld_rmw(mrfld, MRFLD_DSPCHICKENBIT2, 0, MRFLD_CZCLK_UNGATE);
	return 0;
}

int mrfld_mio_island_set(struct mrfld_device *mrfld, bool on)
{
	int ret;

	if (!on)
		return punit_island_set(mrfld, PUNIT_MIO_SS_PM, MIO_SS, false);

	/* The MIPI I/O island has to be cycled once to come up properly */
	ret = punit_island_set(mrfld, PUNIT_MIO_SS_PM, MIO_SS, true);
	if (!ret)
		ret = punit_island_set(mrfld, PUNIT_MIO_SS_PM, MIO_SS, false);
	if (!ret)
		ret = punit_island_set(mrfld, PUNIT_MIO_SS_PM, MIO_SS, true);
	return ret;
}

/* Anniedale runs its display core at 333 MHz */
void mrfld_set_display_clock(struct mrfld_device *mrfld)
{
	mbi_write(CCK_PORT, CCK_FREQ_CNTRL5,
		  CK_ESC_GATE_EN | CK_DP1X_GATE_EN | DISPLAY_FREQ_EN | DISPLAY_FREQ_333MHZ);
}

/*
 * DSI PLL feedback divider: m is not written as is but as an index into the
 * PLL's divider sequence. Values for m = 80..120, the range usable with the
 * 19.2 MHz reference.
 */
#define DSI_PLL_REFCLK_KHZ	19200
#define DSI_PLL_M_MIN		80
#define DSI_PLL_M_MAX		120
#define DSI_PLL_P1_MIN		2
#define DSI_PLL_P1_MAX		6

static const u16 dsi_pll_m_conv[DSI_PLL_M_MAX - DSI_PLL_M_MIN + 1] = {
	213,						/* 80 */
	106,  53, 282, 397, 354, 227, 113,  56, 284, 142,	/* 81 - 90 */
	 71,  35, 273, 136, 324, 418, 465, 488, 500, 506,	/* 91 - 100 */
	253, 126,  63, 287, 399, 455, 483, 241, 376, 444,	/* 101 - 110 */
	478, 495, 503, 251, 381, 446, 479, 239, 375, 443,	/* 111 - 120 */
};

int mrfld_dsi_pll_enable(struct mrfld_device *mrfld, int lane_rate_khz)
{
	int best_m = 0, best_p1 = 0, best_err = INT_MAX;
	int m, p1;
	u32 ctrl;
	int ret;

	for (m = DSI_PLL_M_MIN; m <= DSI_PLL_M_MAX; m++) {
		for (p1 = DSI_PLL_P1_MIN; p1 <= DSI_PLL_P1_MAX; p1++) {
			int err = abs(DSI_PLL_REFCLK_KHZ * m / p1 - lane_rate_khz);

			if (err < best_err) {
				best_err = err;
				best_m = m;
				best_p1 = p1;
			}
		}
	}

	drm_dbg_kms(&mrfld->drm, "DSI PLL: %d kHz wanted, m %d p1 %d gives %d kHz\n",
		    lane_rate_khz, best_m, best_p1, DSI_PLL_REFCLK_KHZ * best_m / best_p1);

	/* Stop the PLL and its clocks before reprogramming */
	mbi_write(CCK_PORT, CCK_DSI_PLL_DIV, 0);
	ctrl = mbi_read(CCK_PORT, CCK_DSI_PLL_CTRL);
	ctrl &= ~(DSI_PLL_VCO_EN | DSI_PLL_LDO_EN | DSI_PLL_CLK_EN_MASK |
		  DSI_PLL_MUX_CCK_DSI0 | DSI_PLL_MUX_CCK_DSI1);
	mbi_write(CCK_PORT, CCK_DSI_PLL_CTRL, ctrl);
	udelay(1);

	mbi_write(CCK_PORT, CCK_DSI_PLL_DIV, dsi_pll_m_conv[best_m - DSI_PLL_M_MIN]);
	ctrl &= ~DSI_PLL_P1_MASK;
	ctrl |= DSI_PLL_P1(best_p1) | DSI_PLL_CLK_EN_DSI0 | DSI_PLL_VCO_EN;
	mbi_write(CCK_PORT, CCK_DSI_PLL_CTRL, ctrl);

	ret = read_poll_timeout(mbi_read, ctrl, ctrl & DSI_PLL_LOCK, 3, 30000, false,
				CCK_PORT, CCK_DSI_PLL_CTRL);
	if (ret)
		drm_err(&mrfld->drm, "DSI PLL did not lock (%#x)\n", ctrl);
	return ret;
}

int mrfld_dsi_pll_disable(struct mrfld_device *mrfld)
{
	u32 ctrl;

	mbi_write(CCK_PORT, CCK_DSI_PLL_DIV, 0);
	ctrl = mbi_read(CCK_PORT, CCK_DSI_PLL_CTRL);
	ctrl &= ~DSI_PLL_CLK_EN_MASK;
	mbi_write(CCK_PORT, CCK_DSI_PLL_CTRL, ctrl);
	udelay(1);
	ctrl &= ~DSI_PLL_VCO_EN;
	ctrl |= DSI_PLL_LDO_EN;
	mbi_write(CCK_PORT, CCK_DSI_PLL_CTRL, ctrl);
	udelay(1);

	if (mbi_read(CCK_PORT, CCK_DSI_PLL_CTRL) & DSI_PLL_LOCK) {
		drm_err(&mrfld->drm, "DSI PLL did not unlock\n");
		return -EBUSY;
	}
	return 0;
}
