// SPDX-License-Identifier: GPL-2.0
/*
 * extcon driver for Basin Cove and Shady Cove PMICs
 *
 * Copyright (c) 2019, Intel Corporation.
 * Author: Andy Shevchenko <andriy.shevchenko@linux.intel.com>
 */

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/devm-helpers.h>
#include <linux/extcon-provider.h>
#include <linux/interrupt.h>
#include <linux/jiffies.h>
#include <linux/mfd/intel_soc_pmic.h>
#include <linux/mfd/intel_soc_pmic_mrfld.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/string_choices.h>
#include <linux/workqueue.h>

#include "extcon-intel.h"

#define BCOVE_USBIDCTRL			0x19
#define BCOVE_USBIDCTRL_ID		BIT(0)
#define BCOVE_USBIDCTRL_ACA		BIT(1)
#define BCOVE_USBIDCTRL_ALL	(BCOVE_USBIDCTRL_ID | BCOVE_USBIDCTRL_ACA)

#define BCOVE_USBIDSTS			0x1a
#define BCOVE_USBIDSTS_GND		BIT(0)
#define BCOVE_USBIDSTS_RARBRC_MASK	GENMASK(2, 1)
#define BCOVE_USBIDSTS_RARBRC_SHIFT	1
#define BCOVE_USBIDSTS_NO_ACA		0
#define BCOVE_USBIDSTS_R_ID_A		1
#define BCOVE_USBIDSTS_R_ID_B		2
#define BCOVE_USBIDSTS_R_ID_C		3
#define BCOVE_USBIDSTS_FLOAT		BIT(3)
#define BCOVE_USBIDSTS_SHORT		BIT(4)

#define BCOVE_CHGRIRQ_ALL	(BCOVE_CHGRIRQ_VBUSDET | BCOVE_CHGRIRQ_DCDET | \
				 BCOVE_CHGRIRQ_BATTDET | BCOVE_CHGRIRQ_USBIDDET)

#define BCOVE_CHGRCTRL0			0x4b
#define BCOVE_CHGRCTRL0_CHGRRESET	BIT(0)
#define BCOVE_CHGRCTRL0_EMRGCHREN	BIT(1)
#define BCOVE_CHGRCTRL0_EXTCHRDIS	BIT(2)
#define BCOVE_CHGRCTRL0_SWCONTROL	BIT(3)
#define BCOVE_CHGRCTRL0_TTLCK		BIT(4)
#define BCOVE_CHGRCTRL0_BIT_5		BIT(5)
#define BCOVE_CHGRCTRL0_BIT_6		BIT(6)
#define BCOVE_CHGRCTRL0_CHR_WDT_NOKICK	BIT(7)

/*
 * Shady Cove on Moorefield phones has no VBUS boost of its own (HACK? Might not be the case for all devices):
 * the 5V for host mode comes from the external SMB1357 charger in OTG mode. The PMIC is
 * told it is the OTG source, and PMIC GPIO6 drives the charger's OTG enable
 * pin.
 */
#define SCOVE_CHGRCTRL1			0x4c
#define SCOVE_CHGRCTRL1_OTGMODE		BIT(6)
#define SCOVE_GPIO6CTLO			0x84
#define SCOVE_GPIO6CTLO_OTG_EN		(BIT(5) | BIT(4) | BIT(0))

/*
 * The boost shuts down on over-current, e.g. on the inrush of a hub or device
 * plugged into a live port, and stays off until its enable is cycled.
 */
#define SCOVE_VBUS_RETRY_MIN_MS		500
#define SCOVE_VBUS_RETRY_MAX_MS		8000
#define SCOVE_VBUS_STABLE_MS		10000

struct mrfld_extcon_data {
	struct device *dev;
	struct regmap *regmap;
	struct extcon_dev *edev;
	unsigned int status;
	unsigned int id;
	unsigned int usbiddet;
	bool host;
	struct mutex vbus_lock;
	struct delayed_work vbus_work;
	unsigned int vbus_retry_ms;
	unsigned long vbus_trip;
};

static const unsigned int mrfld_extcon_cable[] = {
	EXTCON_USB,
	EXTCON_USB_HOST,
	EXTCON_CHG_USB_SDP,
	EXTCON_CHG_USB_CDP,
	EXTCON_CHG_USB_DCP,
	EXTCON_CHG_USB_ACA,
	EXTCON_NONE,
};

static int mrfld_extcon_clear(struct mrfld_extcon_data *data, unsigned int reg,
			      unsigned int mask)
{
	return regmap_update_bits(data->regmap, reg, mask, 0x00);
}

static int mrfld_extcon_set(struct mrfld_extcon_data *data, unsigned int reg,
			    unsigned int mask)
{
	return regmap_update_bits(data->regmap, reg, mask, 0xff);
}

static int mrfld_extcon_sw_control(struct mrfld_extcon_data *data, bool enable)
{
	unsigned int mask = BCOVE_CHGRCTRL0_SWCONTROL;
	struct device *dev = data->dev;
	int ret;

	if (enable)
		ret = mrfld_extcon_set(data, BCOVE_CHGRCTRL0, mask);
	else
		ret = mrfld_extcon_clear(data, BCOVE_CHGRCTRL0, mask);
	if (ret)
		dev_err(dev, "can't set SW control: %d\n", ret);
	return ret;
}

static bool mrfld_extcon_is_scove(struct mrfld_extcon_data *data)
{
	return BCOVE_VENDOR(data->id) == BCOVE_VENDOR_SCOVE;
}

static int scove_extcon_get_id(struct mrfld_extcon_data *data)
{
	unsigned int status;
	int ret;

	ret = regmap_read(data->regmap, BCOVE_SCHGRIRQ1, &status);
	if (ret)
		return ret;

	switch (FIELD_GET(SCOVE_CHGRIRQ_USBIDDET, status)) {
	case SCOVE_USBIDDET_GND:
		return INTEL_USB_ID_GND;
	case SCOVE_USBIDDET_RID:
		/*
		 * Telling RID_A, RID_B and RID_C apart needs the USBID channel
		 * of the GPADC, which is not supported. Stay in device role.
		 */
	default:
		return INTEL_USB_ID_FLOAT;
	}
}

static int mrfld_extcon_get_id(struct mrfld_extcon_data *data)
{
	struct regmap *regmap = data->regmap;
	unsigned int id;
	bool ground;
	int ret;

	if (mrfld_extcon_is_scove(data))
		return scove_extcon_get_id(data);

	ret = regmap_read(regmap, BCOVE_USBIDSTS, &id);
	if (ret)
		return ret;

	if (id & BCOVE_USBIDSTS_FLOAT)
		return INTEL_USB_ID_FLOAT;

	switch ((id & BCOVE_USBIDSTS_RARBRC_MASK) >> BCOVE_USBIDSTS_RARBRC_SHIFT) {
	case BCOVE_USBIDSTS_R_ID_A:
		return INTEL_USB_RID_A;
	case BCOVE_USBIDSTS_R_ID_B:
		return INTEL_USB_RID_B;
	case BCOVE_USBIDSTS_R_ID_C:
		return INTEL_USB_RID_C;
	}

	/*
	 * PMIC A0 reports USBIDSTS_GND = 1 for ID_GND,
	 * but PMIC B0 reports USBIDSTS_GND = 0 for ID_GND.
	 * Thus we must check this bit at last.
	 */
	ground = id & BCOVE_USBIDSTS_GND;
	switch ('A' + BCOVE_MAJOR(data->id)) {
	case 'A':
		return ground ? INTEL_USB_ID_GND : INTEL_USB_ID_FLOAT;
	case 'B':
		return ground ? INTEL_USB_ID_FLOAT : INTEL_USB_ID_GND;
	}

	/* Unknown or unsupported type */
	return INTEL_USB_ID_FLOAT;
}

static int scove_extcon_set_vbus(struct mrfld_extcon_data *data, bool on)
{
	struct regmap *regmap = data->regmap;
	int ret;

	if (on) {
		ret = regmap_set_bits(regmap, SCOVE_CHGRCTRL1, SCOVE_CHGRCTRL1_OTGMODE);
		if (!ret)
			ret = regmap_set_bits(regmap, SCOVE_GPIO6CTLO, SCOVE_GPIO6CTLO_OTG_EN);
	} else {
		ret = regmap_clear_bits(regmap, SCOVE_GPIO6CTLO, SCOVE_GPIO6CTLO_OTG_EN);
		if (!ret)
			ret = regmap_clear_bits(regmap, SCOVE_CHGRCTRL1, SCOVE_CHGRCTRL1_OTGMODE);
	}
	if (ret)
		dev_err(data->dev, "can't turn VBUS %s: %d\n", str_on_off(on), ret);
	return ret;
}

static void scove_extcon_vbus_work(struct work_struct *work)
{
	struct mrfld_extcon_data *data =
		container_of(work, struct mrfld_extcon_data, vbus_work.work);

	guard(mutex)(&data->vbus_lock);

	if (!data->host)
		return;

	scove_extcon_set_vbus(data, false);
	msleep(20);
	scove_extcon_set_vbus(data, true);
}

/* VBUS dropped while we are the host: the boost tripped, restart it. */
static void scove_extcon_vbus_lost(struct mrfld_extcon_data *data)
{
	guard(mutex)(&data->vbus_lock);

	if (!data->host)
		return;

	if (time_after(jiffies, data->vbus_trip + msecs_to_jiffies(SCOVE_VBUS_STABLE_MS)))
		data->vbus_retry_ms = SCOVE_VBUS_RETRY_MIN_MS;
	else
		data->vbus_retry_ms = min(data->vbus_retry_ms * 2, SCOVE_VBUS_RETRY_MAX_MS);
	data->vbus_trip = jiffies;

	dev_warn(data->dev, "VBUS lost in host mode (boost over-current?), restarting in %u ms\n",
		 data->vbus_retry_ms);
	mod_delayed_work(system_wq, &data->vbus_work, msecs_to_jiffies(data->vbus_retry_ms));
}

static int mrfld_extcon_role_detect(struct mrfld_extcon_data *data)
{
	bool scove = mrfld_extcon_is_scove(data);
	unsigned int id;
	bool usb_host;
	int ret;

	ret = mrfld_extcon_get_id(data);
	if (ret < 0)
		return ret;

	id = ret;

	usb_host = (id == INTEL_USB_ID_GND) || (id == INTEL_USB_RID_A);

	/* VBUS goes up before the host starts and down after it stops */
	if (scove) {
		guard(mutex)(&data->vbus_lock);

		data->host = usb_host;
		if (usb_host) {
			data->vbus_retry_ms = SCOVE_VBUS_RETRY_MIN_MS;
			scove_extcon_set_vbus(data, true);
		} else {
			cancel_delayed_work(&data->vbus_work);
		}
	}

	extcon_set_state_sync(data->edev, EXTCON_USB_HOST, usb_host);

	if (scove && !usb_host) {
		guard(mutex)(&data->vbus_lock);

		if (!data->host)
			scove_extcon_set_vbus(data, false);
	}

	return 0;
}

static int mrfld_extcon_cable_detect(struct mrfld_extcon_data *data)
{
	struct regmap *regmap = data->regmap;
	unsigned int status, change;
	int ret;

	/*
	 * It seems SCU firmware clears the content of BCOVE_CHGRIRQ1
	 * and makes it useless for OS. Instead we compare a previously
	 * stored status to the current one, provided by BCOVE_SCHGRIRQ1.
	 */
	ret = regmap_read(regmap, BCOVE_SCHGRIRQ1, &status);
	if (ret)
		return ret;

	change = status ^ data->status;
	if (!change)
		return -ENODATA;

	if (change & data->usbiddet) {
		ret = mrfld_extcon_role_detect(data);
		if (ret)
			return ret;
	} else if (mrfld_extcon_is_scove(data) &&
		   (change & BCOVE_CHGRIRQ_VBUSDET) &&
		   !(status & BCOVE_CHGRIRQ_VBUSDET)) {
		scove_extcon_vbus_lost(data);
	}

	data->status = status;

	return 0;
}

static irqreturn_t mrfld_extcon_interrupt(int irq, void *dev_id)
{
	struct mrfld_extcon_data *data = dev_id;
	int ret;

	ret = mrfld_extcon_cable_detect(data);

	mrfld_extcon_clear(data, BCOVE_MIRQLVL1, BCOVE_LVL1_CHGR);

	return ret ? IRQ_NONE: IRQ_HANDLED;
}

static int mrfld_extcon_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct intel_soc_pmic *pmic = dev_get_drvdata(dev->parent);
	struct regmap *regmap = pmic->regmap;
	struct mrfld_extcon_data *data;
	unsigned int status;
	unsigned int id;
	int irq, ret;

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->dev = dev;
	data->regmap = regmap;

	ret = devm_mutex_init(dev, &data->vbus_lock);
	if (ret)
		return ret;

	ret = devm_delayed_work_autocancel(dev, &data->vbus_work, scove_extcon_vbus_work);
	if (ret)
		return ret;

	data->edev = devm_extcon_dev_allocate(dev, mrfld_extcon_cable);
	if (IS_ERR(data->edev))
		return PTR_ERR(data->edev);

	ret = devm_extcon_dev_register(dev, data->edev);
	if (ret < 0)
		return dev_err_probe(dev, ret, "can't register extcon device\n");

	ret = devm_request_threaded_irq(dev, irq, NULL, mrfld_extcon_interrupt,
					IRQF_ONESHOT | IRQF_SHARED, pdev->name,
					data);
	if (ret)
		return dev_err_probe(dev, ret, "can't register IRQ handler\n");

	ret = regmap_read(regmap, BCOVE_ID, &id);
	if (ret)
		return dev_err_probe(dev, ret, "can't read PMIC ID\n");

	data->id = id;

	if (mrfld_extcon_is_scove(data))
		data->usbiddet = SCOVE_CHGRIRQ_USBIDDET;
	else
		data->usbiddet = BCOVE_CHGRIRQ_USBIDDET;

	ret = mrfld_extcon_sw_control(data, true);
	if (ret)
		return ret;

	/* Get initial state */
	mrfld_extcon_role_detect(data);

	/*
	 * Cached status value is used for cable detection, see comments
	 * in mrfld_extcon_cable_detect(), we need to sync cached value
	 * with a real state of the hardware.
	 */
	regmap_read(regmap, BCOVE_SCHGRIRQ1, &status);
	data->status = status;

	mrfld_extcon_clear(data, BCOVE_MIRQLVL1, BCOVE_LVL1_CHGR);
	mrfld_extcon_clear(data, BCOVE_MCHGRIRQ1, BCOVE_CHGRIRQ_ALL | data->usbiddet);

	mrfld_extcon_set(data, BCOVE_USBIDCTRL, BCOVE_USBIDCTRL_ALL);

	platform_set_drvdata(pdev, data);

	return 0;
}

static void mrfld_extcon_remove(struct platform_device *pdev)
{
	struct mrfld_extcon_data *data = platform_get_drvdata(pdev);

	mrfld_extcon_sw_control(data, false);
}

static const struct platform_device_id mrfld_extcon_id_table[] = {
	{ .name = "mrfld_bcove_pwrsrc" },
	{}
};
MODULE_DEVICE_TABLE(platform, mrfld_extcon_id_table);

static struct platform_driver mrfld_extcon_driver = {
	.driver = {
		.name	= "mrfld_bcove_pwrsrc",
	},
	.probe		= mrfld_extcon_probe,
	.remove		= mrfld_extcon_remove,
	.id_table	= mrfld_extcon_id_table,
};
module_platform_driver(mrfld_extcon_driver);

MODULE_AUTHOR("Andy Shevchenko <andriy.shevchenko@linux.intel.com>");
MODULE_DESCRIPTION("extcon driver for Intel Merrifield Basin Cove and Moorefield Shady Cove PMICs");
MODULE_LICENSE("GPL v2");
