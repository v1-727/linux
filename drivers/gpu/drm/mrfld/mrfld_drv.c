// SPDX-License-Identifier: GPL-2.0-only
/*
 * Intel Merrifield/Moorefield display controller driver
 *
 * Drives pipe A and its MIPI DSI video mode panel on the graphics PCI
 * function. The PowerVR GPU on the same function is left alone.
 */

#include <linux/aperture.h>
#include <linux/dma-mapping.h>
#include <linux/module.h>
#include <linux/pci.h>

#include <drm/clients/drm_client_setup.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_fbdev_shmem.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_module.h>
#include <drm/drm_print.h>

#include "mrfld_drv.h"

#define DRIVER_NAME	"mrfld"
#define DRIVER_DESC	"Intel Merrifield/Moorefield display"

#define PCI_DEVICE_ID_INTEL_MOFLD_GFX	0x1480

DEFINE_DRM_GEM_FOPS(mrfld_fops);

static const struct drm_driver mrfld_driver = {
	.driver_features	= DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC,
	.fops			= &mrfld_fops,
	.name			= DRIVER_NAME,
	.desc			= DRIVER_DESC,
	.major			= 1,
	.minor			= 0,
	.gem_create_object	= mrfld_gem_create_object,
	.gem_prime_import	= drm_gem_shmem_prime_import_no_map,
	.dumb_create		= mrfld_gem_dumb_create,
	DRM_FBDEV_SHMEM_DRIVER_OPS,
};

static void mrfld_dsi_fini_action(struct drm_device *drm, void *arg)
{
	mrfld_dsi_host_fini(to_mrfld(drm));
}

static void mrfld_free_irq_vectors(void *data)
{
	pci_free_irq_vectors(data);
}

static int mrfld_pci_probe(struct pci_dev *pdev, const struct pci_device_id *ent)
{
	struct mrfld_device *mrfld;
	struct drm_device *drm;
	int irq, ret;

	mrfld = devm_drm_dev_alloc(&pdev->dev, &mrfld_driver, struct mrfld_device, drm);
	if (IS_ERR(mrfld))
		return PTR_ERR(mrfld);
	drm = &mrfld->drm;
	mrfld->pdev = pdev;
	spin_lock_init(&mrfld->irq_lock);
	pci_set_drvdata(pdev, drm);

	ret = pcim_enable_device(pdev);
	if (ret)
		return ret;
	pci_set_master(pdev);

	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;

	/*
	 * The graphics function's INTx line is not wired: PCI_INTERRUPT_LINE
	 * reads 0, so the Intel MID IRQ code leaves pdev->irq unset (0 is the
	 * eMMC's bogus GSI) and request_irq() on it would fail. Take a MSI
	 * vector instead, as the downstream driver does.
	 */
	ret = pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_MSI);
	if (ret < 0)
		return dev_err_probe(&pdev->dev, ret, "no usable interrupt\n");
	ret = devm_add_action_or_reset(&pdev->dev, mrfld_free_irq_vectors, pdev);
	if (ret)
		return ret;

	mrfld->mmio = pcim_iomap_region(pdev, 0, DRIVER_NAME);
	if (IS_ERR(mrfld->mmio))
		return PTR_ERR(mrfld->mmio);

	ret = mrfld_power_init(mrfld);
	if (ret)
		return ret;

	ret = mrfld_gtt_init(mrfld);
	if (ret)
		return ret;

	ret = mrfld_panel_init(mrfld);
	if (ret)
		return ret;

	ret = mrfld_dsi_host_init(mrfld);
	if (ret)
		return ret;
	ret = drmm_add_action_or_reset(drm, mrfld_dsi_fini_action, NULL);
	if (ret)
		return ret;

	ret = mrfld_kms_init(mrfld);
	if (ret)
		return ret;

	/*
	 * Everything is in place: take the display from the firmware. Its
	 * framebuffer is in stolen memory, outside of our BARs.
	 */
	ret = aperture_remove_all_conflicting_devices(mrfld_driver.name);
	if (ret)
		return ret;
	mrfld_hw_takeover(mrfld);

	irq = pci_irq_vector(pdev, 0);
	drm_dbg_driver(drm, "interrupt on MSI vector %d\n", irq);
	ret = devm_request_irq(&pdev->dev, irq, mrfld_irq_handler,
			       IRQF_SHARED, DRIVER_NAME, mrfld);
	if (ret)
		return ret;

	ret = drm_dev_register(drm, 0);
	if (ret)
		return ret;

	drm_client_setup(drm, NULL);
	return 0;
}

static void mrfld_pci_remove(struct pci_dev *pdev)
{
	struct drm_device *drm = pci_get_drvdata(pdev);

	drm_dev_unplug(drm);
	drm_atomic_helper_shutdown(drm);
}

static void mrfld_pci_shutdown(struct pci_dev *pdev)
{
	drm_atomic_helper_shutdown(pci_get_drvdata(pdev));
}

static const struct pci_device_id mrfld_pci_ids[] = {
	{ PCI_VDEVICE(INTEL, PCI_DEVICE_ID_INTEL_MOFLD_GFX) },
	{ }
};
MODULE_DEVICE_TABLE(pci, mrfld_pci_ids);

static struct pci_driver mrfld_pci_driver = {
	.name = DRIVER_NAME,
	.id_table = mrfld_pci_ids,
	.probe = mrfld_pci_probe,
	.remove = mrfld_pci_remove,
	.shutdown = mrfld_pci_shutdown,
};

drm_module_pci_driver(mrfld_pci_driver);

MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("GPL");
