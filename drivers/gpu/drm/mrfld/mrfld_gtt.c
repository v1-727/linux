// SPDX-License-Identifier: GPL-2.0-only
/*
 * Intel Merrifield/Moorefield display GTT
 *
 * The display controller reaches memory through a graphics translation
 * table kept in RAM: one 32-bit entry per 4 KiB page, (pfn << 12) | valid,
 * so it can only map memory below 4 GiB. Firmware keeps its stolen memory,
 * where the boot framebuffer lives, mapped at the start; scanout buffers are
 * mapped behind it while they are pinned for display.
 */

#include <linux/bitfield.h>
#include <linux/gfp.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/pagemap.h>
#include <linux/pci.h>
#include <linux/sizes.h>
#include <linux/slab.h>

#include <drm/drm_dumb_buffers.h>
#include <drm/drm_gem.h>
#include <drm/drm_managed.h>
#include <drm/drm_print.h>

#include "mrfld_drv.h"

static u32 mrfld_pte(unsigned long pfn)
{
	return (pfn << PAGE_SHIFT) | MRFLD_PTE_VALID;
}

static void mrfld_gtt_fini(struct drm_device *drm, void *arg)
{
	struct mrfld_device *mrfld = to_mrfld(drm);

	drm_mm_takedown(&mrfld->gtt_mm);
	__free_page(mrfld->scratch_page);
}

int mrfld_gtt_init(struct mrfld_device *mrfld)
{
	struct pci_dev *pdev = mrfld->pdev;
	struct drm_device *drm = &mrfld->drm;
	u32 gtt_base, stolen_base, msac;
	u32 scratch_pte;
	unsigned int i;
	u64 aperture;

	pci_read_config_dword(pdev, MRFLD_PCI_BGSM, &gtt_base);
	pci_read_config_dword(pdev, MRFLD_PCI_BSM, &stolen_base);
	pci_read_config_dword(pdev, MRFLD_PCI_MSAC, &msac);
	gtt_base &= PAGE_MASK;
	stolen_base &= PAGE_MASK;

	switch (FIELD_GET(MRFLD_MSAC_APERTURE, msac)) {
	case 0:
		aperture = SZ_1G;
		break;
	case 1:
		aperture = SZ_512M;
		break;
	case 2:
		aperture = SZ_256M;
		break;
	default:
		drm_err(drm, "unknown aperture size, MSAC %#x\n", msac);
		return -ENODEV;
	}

	if (!gtt_base || stolen_base >= gtt_base) {
		drm_err(drm, "bad GTT %#x / stolen memory %#x\n", gtt_base, stolen_base);
		return -ENODEV;
	}

	mrfld->gtt_entries = aperture >> PAGE_SHIFT;
	/* Stolen memory runs up to the GTT, less one page */
	mrfld->stolen_entries = (gtt_base - stolen_base - PAGE_SIZE) >> PAGE_SHIFT;
	if (mrfld->stolen_entries >= mrfld->gtt_entries)
		return -ENODEV;

	drm_info(drm, "GTT at %#x, %u MiB aperture, %u MiB stolen at %#x\n",
		 gtt_base, (u32)(aperture >> 20), mrfld->stolen_entries >> 8, stolen_base);

	mrfld->gtt = devm_ioremap(&pdev->dev, gtt_base, mrfld->gtt_entries * sizeof(u32));
	if (!mrfld->gtt)
		return -ENOMEM;

	mrfld->scratch_page = alloc_page(GFP_KERNEL | GFP_DMA32 | __GFP_ZERO);
	if (!mrfld->scratch_page)
		return -ENOMEM;

	/* Everything but stolen memory points at the scratch page */
	scratch_pte = mrfld_pte(page_to_pfn(mrfld->scratch_page));
	for (i = mrfld->stolen_entries; i < mrfld->gtt_entries; i++)
		writel(scratch_pte, mrfld->gtt + i * sizeof(u32));
	readl(mrfld->gtt + (i - 1) * sizeof(u32));

	drm_mm_init(&mrfld->gtt_mm, mrfld->stolen_entries,
		    mrfld->gtt_entries - mrfld->stolen_entries);
	mutex_init(&mrfld->gtt_lock);

	return drmm_add_action_or_reset(drm, mrfld_gtt_fini, NULL);
}

static void mrfld_gem_free(struct drm_gem_object *obj)
{
	struct mrfld_gem_object *bo = to_mrfld_gem(obj);

	drm_WARN_ON(obj->dev, bo->gtt_pin_count);
	drm_gem_shmem_object_free(obj);
}

static const struct drm_gem_object_funcs mrfld_gem_funcs = {
	.free = mrfld_gem_free,
	.print_info = drm_gem_shmem_object_print_info,
	.pin = drm_gem_shmem_object_pin,
	.unpin = drm_gem_shmem_object_unpin,
	.get_sg_table = drm_gem_shmem_object_get_sg_table,
	.vmap = drm_gem_shmem_object_vmap,
	.vunmap = drm_gem_shmem_object_vunmap,
	.mmap = drm_gem_shmem_object_mmap,
	.vm_ops = &drm_gem_shmem_vm_ops,
};

struct drm_gem_object *mrfld_gem_create_object(struct drm_device *drm, size_t size)
{
	struct mrfld_gem_object *bo;

	bo = kzalloc_obj(*bo);
	if (!bo)
		return ERR_PTR(-ENOMEM);

	/* The display controller does not snoop CPU caches */
	bo->base.map_wc = true;
	bo->base.base.funcs = &mrfld_gem_funcs;

	return &bo->base.base;
}

int mrfld_gem_dumb_create(struct drm_file *file, struct drm_device *drm,
			  struct drm_mode_create_dumb *args)
{
	struct drm_gem_shmem_object *shmem;
	int ret;

	/* Plane strides are in units of 64 bytes */
	ret = drm_mode_size_dumb(drm, args, 64, 0);
	if (ret)
		return ret;

	shmem = drm_gem_shmem_create(drm, args->size);
	if (IS_ERR(shmem))
		return PTR_ERR(shmem);

	/* GTT entries hold 32-bit addresses: keep the pages below 4 GiB */
	mapping_set_gfp_mask(shmem->base.filp->f_mapping,
			     GFP_USER | __GFP_DMA32 | __GFP_RETRY_MAYFAIL | __GFP_NOWARN);

	ret = drm_gem_handle_create(file, &shmem->base, &args->handle);
	drm_gem_object_put(&shmem->base);
	return ret;
}

/**
 * mrfld_gem_gtt_pin - map a buffer for scanout
 * @mrfld: device
 * @obj: buffer
 * @offset: returns the buffer's graphics address
 *
 * Pins the buffer's pages and maps them in the GTT, once; later calls only
 * count.
 */
int mrfld_gem_gtt_pin(struct mrfld_device *mrfld, struct drm_gem_object *obj, u32 *offset)
{
	struct mrfld_gem_object *bo;
	struct drm_gem_shmem_object *shmem;
	unsigned int npages, i;
	int ret;

	/* imported buffers have no shmem pages to map */
	if (obj->funcs != &mrfld_gem_funcs || drm_gem_is_imported(obj))
		return -EINVAL;

	bo = to_mrfld_gem(obj);
	shmem = &bo->base;
	npages = obj->size >> PAGE_SHIFT;

	guard(mutex)(&mrfld->gtt_lock);

	if (bo->gtt_pin_count) {
		bo->gtt_pin_count++;
		*offset = bo->gtt_node.start << PAGE_SHIFT;
		return 0;
	}

	ret = drm_gem_shmem_pin(shmem);
	if (ret)
		return ret;

	for (i = 0; i < npages; i++) {
		if (page_to_pfn(shmem->pages[i]) >= BIT(32 - PAGE_SHIFT)) {
			drm_dbg_kms(&mrfld->drm, "buffer page above 4 GiB\n");
			ret = -EINVAL;
			goto err_unpin;
		}
	}

	ret = drm_mm_insert_node(&mrfld->gtt_mm, &bo->gtt_node, npages);
	if (ret)
		goto err_unpin;

	for (i = 0; i < npages; i++)
		writel(mrfld_pte(page_to_pfn(shmem->pages[i])),
		       mrfld->gtt + (bo->gtt_node.start + i) * sizeof(u32));
	readl(mrfld->gtt + (bo->gtt_node.start + i - 1) * sizeof(u32));

	bo->gtt_pin_count = 1;
	*offset = bo->gtt_node.start << PAGE_SHIFT;
	return 0;

err_unpin:
	drm_gem_shmem_unpin(shmem);
	return ret;
}

void mrfld_gem_gtt_unpin(struct mrfld_device *mrfld, struct drm_gem_object *obj)
{
	struct mrfld_gem_object *bo = to_mrfld_gem(obj);
	u32 scratch_pte = mrfld_pte(page_to_pfn(mrfld->scratch_page));
	unsigned int i;

	guard(mutex)(&mrfld->gtt_lock);

	if (drm_WARN_ON(obj->dev, !bo->gtt_pin_count) || --bo->gtt_pin_count)
		return;

	for (i = 0; i < bo->gtt_node.size; i++)
		writel(scratch_pte, mrfld->gtt + (bo->gtt_node.start + i) * sizeof(u32));
	readl(mrfld->gtt + bo->gtt_node.start * sizeof(u32));

	drm_mm_remove_node(&bo->gtt_node);
	drm_gem_shmem_unpin(&bo->base);
}
