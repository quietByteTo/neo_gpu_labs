// SPDX-License-Identifier: GPL-2.0-only
/*
 * neogpu_mem.c - NeoGPU Memory Management Implementation
 * 
 * Buddy Allocator for 4GB HBM VRAM management.
 * Supports direct BAR1 CPU access (for small buffers) and
 * Copy Engine transfer (for large buffers).
 */

#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/scatterlist.h>
#include <linux/dma-buf.h>

#include "neogpu_mem.h"
#include "neogpu_drv.h"
#include "neogpu_hw.h"

#define BAR1_WINDOW_SIZE	(256 * 1024 * 1024ULL)  /* Map first 256MB for CPU access */

/* Internal helpers */
static inline unsigned long addr_to_page(u64 addr)
{
	return addr >> NEOGPU_PAGE_SHIFT;
}

static inline u64 page_to_addr(unsigned long page)
{
	return (u64)page << NEOGPU_PAGE_SHIFT;
}

static inline unsigned int size_to_order(size_t size)
{
	unsigned int order = 0;
	size_t pages = (size + NEOGPU_PAGE_SIZE - 1) >> NEOGPU_PAGE_SHIFT;
	
	while ((1UL << order) < pages)
		order++;
	
	return order;
}

/**
 * neogpu_mem_mgr_init - Initialize buddy allocator
 * @gpu: Device pointer
 * 
 * Sets up free lists and bitmap for 4GB VRAM.
 * Return: 0 on success, -ENOMEM on failure
 */
int neogpu_mem_mgr_init(struct neogpu_device *gpu)
{
	struct neogpu_mem_mgr *mgr;
	int i;
	
	mgr = kzalloc(sizeof(*mgr), GFP_KERNEL);
	if (!mgr)
		return -ENOMEM;
	
	mgr->gpu = gpu;
	gpu->mem_mgr = mgr;  /* Add mem_mgr to neogpu_device struct */
	spin_lock_init(&mgr->lock);
	
	mgr->total_pages = NEOGPU_VRAM_SIZE >> NEOGPU_PAGE_SHIFT;
	mgr->free_pages = mgr->total_pages;
	
	/* Initialize free lists */
	for (i = 0; i < NEOGPU_NUM_BUCKETS; i++)
		INIT_LIST_HEAD(&mgr->free_list[i]);
	
	/* Allocate bitmap (1 bit per page) */
	mgr->page_bitmap = vzalloc((mgr->total_pages + 7) / 8);
	if (!mgr->page_bitmap) {
		kfree(mgr);
		return -ENOMEM;
	}
	
	/* Initially, all memory is one big free block at order MAX_ORDER */
	/* Simplified: treat as all free, allocate from bottom up */
	
	dev_info(gpu->dev, "Memory manager initialized: %lu pages (%llu MB)\n",
		 mgr->total_pages, NEOGPU_VRAM_SIZE >> 20);
	
	return 0;
}

/**
 * neogpu_mem_mgr_fini - Cleanup memory manager
 */
void neogpu_mem_mgr_fini(struct neogpu_device *gpu)
{
	struct neogpu_mem_mgr *mgr = gpu->mem_mgr;
	
	if (!mgr)
		return;
	
	/* Check for leaks */
	if (mgr->bytes_allocated != mgr->bytes_freed)
		dev_warn(gpu->dev, "Memory leak detected: %llu bytes unfreed\n",
			 mgr->bytes_allocated - mgr->bytes_freed);
	
	vfree(mgr->page_bitmap);
	kfree(mgr);
	gpu->mem_mgr = NULL;
}

/**
 * alloc_pages_from_bitmap - Simple bitmap allocator (fallback)
 * @mgr: Memory manager
 * @num_pages: Pages to allocate
 * 
 * Finds contiguous free pages using bitmap scan.
 * Return: start page index or -1 on failure
 */
static long alloc_pages_from_bitmap(struct neogpu_mem_mgr *mgr, unsigned int num_pages)
{
	unsigned long start, end, i;
	unsigned long flags;
	
	spin_lock_irqsave(&mgr->lock, flags);
	
	/* Simple first-fit search (inefficient but correct) */
	for (start = 0; start <= mgr->total_pages - num_pages; ) {
		/* Check if range is free */
		for (i = 0; i < num_pages; i++) {
			if (test_bit(start + i, mgr->page_bitmap))
				break;  /* Page in use */
		}
		
		if (i == num_pages) {
			/* Found! Mark as used */
			for (i = 0; i < num_pages; i++)
				set_bit(start + i, mgr->page_bitmap);
			
			mgr->free_pages -= num_pages;
			spin_unlock_irqrestore(&mgr->lock, flags);
			return start;
		}
		
		/* Skip to next potential start (advance by 1 or to next free) */
		start += (i + 1);
	}
	
	spin_unlock_irqrestore(&mgr->lock, flags);
	return -1;  /* No space */
}

/**
 * free_pages_to_bitmap - Free pages back to allocator
 */
static void free_pages_to_bitmap(struct neogpu_mem_mgr *mgr, unsigned long start, 
				  unsigned int num_pages)
{
	unsigned long flags;
	unsigned int i;
	
	spin_lock_irqsave(&mgr->lock, flags);
	
	for (i = 0; i < num_pages; i++)
		clear_bit(start + i, mgr->page_bitmap);
	
	mgr->free_pages += num_pages;
	
	spin_unlock_irqrestore(&mgr->lock, flags);
}

/**
 * neogpu_bo_alloc - Allocate Buffer Object from VRAM
 * @gpu: Device pointer
 * @size: Requested size (bytes)
 * @flags: Allocation flags (NEOGPU_MEM_*)
 * 
 * Allocates contiguous VRAM region and creates BO structure.
 * If NEOGPU_MEM_CPU_ACCESS flag set, attempts to map via BAR1.
 * 
 * Return: BO pointer or ERR_PTR
 */
struct neogpu_bo *neogpu_bo_alloc(struct neogpu_device *gpu, size_t size, u32 flags)
{
	struct neogpu_mem_mgr *mgr = gpu->mem_mgr;
	struct neogpu_bo *bo;
	unsigned int num_pages;
	long start_page;
	u64 gpu_addr;
	
	if (!mgr)
		return ERR_PTR(-ENODEV);
	
	if (size == 0 || size > NEOGPU_VRAM_SIZE)
		return ERR_PTR(-EINVAL);
	
	/* Round up to page size */
	num_pages = (size + NEOGPU_PAGE_SIZE - 1) >> NEOGPU_PAGE_SHIFT;
	
	bo = kzalloc(sizeof(*bo), GFP_KERNEL);
	if (!bo)
		return ERR_PTR(-ENOMEM);
	
	/* Allocate physical pages */
	start_page = alloc_pages_from_bitmap(mgr, num_pages);
	if (start_page < 0) {
		mgr->allocs_failed++;
		kfree(bo);
		return ERR_PTR(-ENOMEM);
	}
	
	gpu_addr = page_to_addr(start_page);
	
	/* Initialize BO */
	bo->gpu = gpu;
	bo->gpu_addr = gpu_addr;
	bo->size = (u64)num_pages << NEOGPU_PAGE_SHIFT;  /* Aligned size */
	bo->num_pages = num_pages;
	bo->flags = flags;
	bo->bar1_offset = gpu_addr;  /* BAR1 starts at GPU addr 0 */
	bo->bar1_mapped = (gpu_addr + bo->size) <= BAR1_WINDOW_SIZE;
	bo->cpu_ptr = NULL;
	bo->vmap_ptr = NULL;
	bo->fence = NULL;
	bo->busy = false;
	
	refcount_set(&bo->refcount, 1);
	spin_lock_init(&bo->lock);
	INIT_LIST_HEAD(&bo->list);
	
	/* If CPU access requested and within BAR1 window, map it */
	if ((flags & NEOGPU_MEM_CPU_ACCESS) && bo->bar1_mapped) {
		bo->cpu_ptr = gpu->bar1 + bo->bar1_offset;
	}
	
	/* Update stats */
	mgr->bytes_allocated += bo->size;
	
	dev_dbg(gpu->dev, "BO allocated: addr=0x%llx, size=%zu, pages=%u, mapped=%d\n",
		gpu_addr, size, num_pages, bo->bar1_mapped);
	
	return bo;
}

/**
 * neogpu_bo_free - Free Buffer Object and underlying memory
 */
void neogpu_bo_free(struct neogpu_bo *bo)
{
	struct neogpu_device *gpu;
	struct neogpu_mem_mgr *mgr;
	
	if (!bo)
		return;
	
	gpu = bo->gpu;
	mgr = gpu->mem_mgr;
	
	/* Unmap if mapped */
	if (bo->cpu_ptr && bo->bar1_mapped) {
		/* Nothing to do for iomem, just clear pointer */
		bo->cpu_ptr = NULL;
	}
	if (bo->vmap_ptr) {
		vfree(bo->vmap_ptr);
		bo->vmap_ptr = NULL;
	}
	
	/* Free VRAM pages */
	free_pages_to_bitmap(mgr, addr_to_page(bo->gpu_addr), bo->num_pages);
	
	mgr->bytes_freed += bo->size;
	
	dev_dbg(gpu->dev, "BO freed: addr=0x%llx, size=%zu\n", bo->gpu_addr, bo->size);
	
	kfree(bo);
}

/**
 * neogpu_bo_ref - Increment reference count
 */
struct neogpu_bo *neogpu_bo_ref(struct neogpu_bo *bo)
{
	if (bo)
		refcount_inc(&bo->refcount);
	return bo;
}

/**
 * neogpu_bo_unref - Decrement reference count, free if zero
 */
void neogpu_bo_unref(struct neogpu_bo *bo)
{
	if (!bo)
		return;
	
	if (refcount_dec_and_test(&bo->refcount))
		neogpu_bo_free(bo);
}

/**
 * neogpu_bo_kmap - Map BO to kernel virtual address
 * @bo: Buffer object
 * 
 * For BAR1-mapped BOs, returns existing cpu_ptr.
 * For large BOs beyond BAR1, allocates staging buffer (fallback).
 * 
 * Return: 0 on success, -EINVAL if not mappable
 */
int neogpu_bo_kmap(struct neogpu_bo *bo)
{
	if (!bo)
		return -EINVAL;
	
	/* Already mapped via BAR1? */
	if (bo->cpu_ptr)
		return 0;
	
	/* Beyond BAR1 window - need Copy Engine or staging buffer */
	if (bo->flags & NEOGPU_MEM_CE_TRANSFER) {
		/* Allocate kernel buffer for staging */
		bo->vmap_ptr = vmalloc(bo->size);
		if (!bo->vmap_ptr)
			return -ENOMEM;
		return 0;
	}
	
	return -EINVAL;  /* Cannot map */
}

/**
 * neogpu_bo_kunmap - Unmap from kernel
 */
void neogpu_bo_kunmap(struct neogpu_bo *bo)
{
	if (!bo)
		return;
	
	/* BAR1 mapping persists, just staging buffer freed */
	if (bo->vmap_ptr) {
		vfree(bo->vmap_ptr);
		bo->vmap_ptr = NULL;
	}
}

/**
 * neogpu_bo_mmap - Setup user mmap for BO
 * @bo: Buffer object
 * @vma: VMA from user space
 * 
 * Maps BAR1 region to user space (read/write).
 * Only works if BO is within BAR1 window (first 256MB).
 * 
 * Return: 0 on success, -EINVAL if unmappable
 */
int neogpu_bo_mmap(struct neogpu_bo *bo, struct vm_area_struct *vma)
{
	struct neogpu_device *gpu;
	unsigned long pfn;
	int ret;
	
	if (!bo || !vma)
		return -EINVAL;
	
	gpu = bo->gpu;
	
	if (!bo->bar1_mapped) {
		dev_err(gpu->dev, "BO at 0x%llx not in BAR1 window, cannot mmap\n", 
			bo->gpu_addr);
		return -EINVAL;
	}
	
	/* Calculate PFN in BAR1 */
	pfn = (gpu->bar1_start + bo->bar1_offset) >> PAGE_SHIFT;
	
	/* Remap BAR1 PFNs to user space */
	/* Note: pgprot should be write-combining for GPU memory */
	vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);
	
	ret = remap_pfn_range(vma, vma->vm_start, pfn,
			      vma->vm_end - vma->vm_start, vma->vm_page_prot);
	if (ret) {
		dev_err(gpu->dev, "remap_pfn_range failed: %d\n", ret);
		return ret;
	}
	
	dev_dbg(gpu->dev, "BO mmap: user=0x%lx, pfn=0x%lx, size=%zu\n",
		vma->vm_start, pfn, bo->size);
	
	return 0;
}

/**
 * neogpu_bo_memcpy_to_gpu - Copy data from CPU to GPU memory
 * @bo: Destination BO
 * @src: Source CPU buffer
 * @len: Bytes to copy
 * @offset: Offset in BO
 * 
 * Uses direct BAR1 write if mapped, otherwise uses staging buffer
 * (requires subsequent CE upload - not implemented here).
 */
int neogpu_bo_memcpy_to_gpu(struct neogpu_bo *bo, const void *src, 
			     size_t len, loff_t offset)
{
	if (!bo || !src || offset < 0 || offset + len > bo->size)
		return -EINVAL;
	
	if (bo->cpu_ptr) {
		/* Direct BAR1 write */
		memcpy_toio(bo->cpu_ptr + offset, src, len);
		return 0;
	}
	
	if (bo->vmap_ptr) {
		/* Staging buffer - copy here, CE will upload later */
		memcpy((u8 *)bo->vmap_ptr + offset, src, len);
		return 0;
	}
	
	return -EINVAL;
}

/**
 * neogpu_bo_memcpy_from_gpu - Copy data from GPU to CPU memory
 */
int neogpu_bo_memcpy_from_gpu(struct neogpu_bo *bo, void *dst,
			       size_t len, loff_t offset)
{
	if (!bo || !dst || offset < 0 || offset + len > bo->size)
		return -EINVAL;
	
	if (bo->cpu_ptr) {
		memcpy_fromio(dst, bo->cpu_ptr + offset, len);
		return 0;
	}
	
	if (bo->vmap_ptr) {
		/* Staging buffer - data should have been downloaded by CE */
		memcpy(dst, (u8 *)bo->vmap_ptr + offset, len);
		return 0;
	}
	
	return -EINVAL;
}

/**
 * neogpu_bo_clear - Clear GPU memory to value
 */
int neogpu_bo_clear(struct neogpu_bo *bo, int value, size_t len, loff_t offset)
{
	if (!bo || offset < 0 || offset + len > bo->size)
		return -EINVAL;
	
	if (bo->cpu_ptr) {
		memset_io(bo->cpu_ptr + offset, value, len);
		return 0;
	}
	
	/* For non-mapped, would need GPU kernel to clear */
	return -EINVAL;
}

/**
 * neogpu_mem_stats - Print memory statistics
 */
void neogpu_mem_stats(struct neogpu_device *gpu)
{
	struct neogpu_mem_mgr *mgr = gpu->mem_mgr;
	
	if (!mgr)
		return;
	
	dev_info(gpu->dev, "=== Memory Statistics ===\n");
	dev_info(gpu->dev, "Total: %llu MB, Free: %lu MB (%lu%%)\n",
		 NEOGPU_VRAM_SIZE >> 20,
		 (mgr->free_pages * NEOGPU_PAGE_SIZE) >> 20,
		 (mgr->free_pages * 100) / mgr->total_pages);
	dev_info(gpu->dev, "Allocated: %llu MB, Freed: %llu MB\n",
		 mgr->bytes_allocated >> 20,
		 mgr->bytes_freed >> 20);
	dev_info(gpu->dev, "Active: %llu MB, Failed allocs: %u\n",
		 (mgr->bytes_allocated - mgr->bytes_freed) >> 20,
		 mgr->allocs_failed);
}