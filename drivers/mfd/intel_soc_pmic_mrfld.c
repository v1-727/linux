// SPDX-License-Identifier: GPL-2.0
/*
 * Device access for Basin Cove and Shady Cove PMICs
 *
 * Copyright (c) 2019, Intel Corporation.
 * Author: Andy Shevchenko <andriy.shevchenko@linux.intel.com>
 */

#include <linux/acpi.h>
#include <linux/interrupt.h>
#include <linux/mfd/core.h>
#include <linux/mfd/intel_soc_pmic.h>
#include <linux/mfd/intel_soc_pmic_mrfld.h>
#include <linux/module.h>
#include <linux/platform_data/x86/intel_scu_ipc.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/regmap.h>

/*
 * Level 2 IRQs
 *
 * Firmware on the systems with Basin Cove and Shady Cove PMICs services
 * Level 1 IRQs without an assistance. Thus, each of the Level 1 IRQ is
 * represented as a separate RTE in IOAPIC. Both PMICs share this layout.
 */
static struct resource irq_level2_resources[] = {
	DEFINE_RES_IRQ(0), /* power button */
	DEFINE_RES_IRQ(0), /* TMU */
	DEFINE_RES_IRQ(0), /* thermal */
	DEFINE_RES_IRQ(0), /* BCU */
	DEFINE_RES_IRQ(0), /* ADC */
	DEFINE_RES_IRQ(0), /* charger */
	DEFINE_RES_IRQ(0), /* GPIO */
};

static const struct mfd_cell bcove_dev[] = {
	{
		.name = "mrfld_bcove_pwrbtn",
		.num_resources = 1,
		.resources = &irq_level2_resources[0],
	}, {
		.name = "mrfld_bcove_tmu",
		.num_resources = 1,
		.resources = &irq_level2_resources[1],
	}, {
		.name = "mrfld_bcove_thermal",
		.num_resources = 1,
		.resources = &irq_level2_resources[2],
	}, {
		.name = "mrfld_bcove_bcu",
		.num_resources = 1,
		.resources = &irq_level2_resources[3],
	}, {
		.name = "mrfld_bcove_adc",
		.num_resources = 1,
		.resources = &irq_level2_resources[4],
	}, {
		.name = "mrfld_bcove_charger",
		.num_resources = 1,
		.resources = &irq_level2_resources[5],
	}, {
		.name = "mrfld_bcove_pwrsrc",
		.num_resources = 1,
		.resources = &irq_level2_resources[5],
	}, {
		.name = "mrfld_bcove_gpio",
		.num_resources = 1,
		.resources = &irq_level2_resources[6],
	},
	{	.name = "mrfld_bcove_region", },
};

/*
 * Shady Cove on Moorefield keeps only the IRQ, power button and charger detection
 * registers of Basin Cove.
 */
static const struct mfd_cell scove_dev[] = {
	{
		.name = "mrfld_bcove_pwrbtn",
		.num_resources = 1,
		.resources = &irq_level2_resources[0],
	}, {
		.name = "mrfld_bcove_pwrsrc",
		.num_resources = 1,
		.resources = &irq_level2_resources[5],
	},
};

static int bcove_ipc_byte_reg_read(void *context, unsigned int reg,
				    unsigned int *val)
{
	struct intel_soc_pmic *pmic = context;
	u8 ipc_out;
	int ret;

	ret = intel_scu_ipc_dev_ioread8(pmic->scu, reg, &ipc_out);
	if (ret)
		return ret;

	*val = ipc_out;
	return 0;
}

static int bcove_ipc_byte_reg_write(void *context, unsigned int reg,
				     unsigned int val)
{
	struct intel_soc_pmic *pmic = context;
	u8 ipc_in = val;

	return intel_scu_ipc_dev_iowrite8(pmic->scu, reg, ipc_in);
}

static const struct regmap_config bcove_regmap_config = {
	.reg_bits = 16,
	.val_bits = 8,
	.max_register = 0xff,
	.reg_write = bcove_ipc_byte_reg_write,
	.reg_read = bcove_ipc_byte_reg_read,
};

/* Shady Cove has registers above 0xff, e.g. VPROGxCNT at 0x140 */
static const struct regmap_config scove_regmap_config = {
	.reg_bits = 16,
	.val_bits = 8,
	.max_register = 0x1ff,
	.reg_write = bcove_ipc_byte_reg_write,
	.reg_read = bcove_ipc_byte_reg_read,
};

struct bcove_pmic_data {
	const struct mfd_cell *cells;
	unsigned int n_cells;
	const struct regmap_config *regmap_config;
};

static const struct bcove_pmic_data bcove_data = {
	.cells = bcove_dev,
	.n_cells = ARRAY_SIZE(bcove_dev),
	.regmap_config = &bcove_regmap_config,
};

static const struct bcove_pmic_data scove_data = {
	.cells = scove_dev,
	.n_cells = ARRAY_SIZE(scove_dev),
	.regmap_config = &scove_regmap_config,
};

static int bcove_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	const struct bcove_pmic_data *data;
	struct intel_soc_pmic *pmic;
	unsigned int i;
	int ret;

	data = device_get_match_data(dev);
	if (!data)
		return -ENODEV;

	pmic = devm_kzalloc(dev, sizeof(*pmic), GFP_KERNEL);
	if (!pmic)
		return -ENOMEM;

	/* The SCU IPC PCI driver may not be bound yet when built-in */
	pmic->scu = devm_intel_scu_ipc_dev_get(dev);
	if (!pmic->scu)
		return -EPROBE_DEFER;

	platform_set_drvdata(pdev, pmic);
	pmic->dev = &pdev->dev;

	pmic->regmap = devm_regmap_init(dev, NULL, pmic, data->regmap_config);
	if (IS_ERR(pmic->regmap))
		return PTR_ERR(pmic->regmap);

	for (i = 0; i < ARRAY_SIZE(irq_level2_resources); i++) {
		ret = platform_get_irq(pdev, i);
		if (ret < 0)
			return ret;

		irq_level2_resources[i].start = ret;
		irq_level2_resources[i].end = ret;
	}

	return devm_mfd_add_devices(dev, PLATFORM_DEVID_NONE,
				    data->cells, data->n_cells,
				    NULL, 0, NULL);
}

static const struct acpi_device_id bcove_acpi_ids[] = {
	{ "INTC100E", (kernel_ulong_t)&bcove_data },
	{ "INTC100F", (kernel_ulong_t)&scove_data },
	{}
};
MODULE_DEVICE_TABLE(acpi, bcove_acpi_ids);

static struct platform_driver bcove_driver = {
	.driver = {
		.name = "intel_soc_pmic_mrfld",
		.acpi_match_table = bcove_acpi_ids,
	},
	.probe = bcove_probe,
};
module_platform_driver(bcove_driver);

MODULE_DESCRIPTION("IPC driver for Intel SoC Basin Cove and Shady Cove PMICs");
MODULE_LICENSE("GPL v2");
