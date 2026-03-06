/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * neogpu_mem.h - NeoGPU Memory Management Header
 * 
 * Buddy Allocator for 4GB HBM, Buffer Object management,
 * and CPU access interfaces (BAR1 mmap or Copy Engine).
 */

#ifndef _NEOGPU_MEM_H_
#define _NEOGPU_MEM_H_

#include <linux/types.h>
#include <linux/mm.h>
#include <linux/spinlock.h>
#include <linux/list.h>

/* Memory Configuration */
#define NEOGPU_VRAM_SIZE		(4ULL * 1024 * 1024 * 1024)  /* 4GB */
#define NEOGPU_PAGE_SHIFT		12  /* 4KB minimum allocation */
#define NEOGPU_PAGE_SIZE		(1 << NEOGPU_PAGE_SHIFT)
#define NEOGPU_MAX_ORDER		20  /* 4GB = 2^20 * 4KB */
#define NEOGPU_NUM_BUCKETS		(NEOGPU_MAX_ORDER + 1)

/* Allocation flags */
#define NEOGPU_MEM_KERNEL		BIT(0)  /* Kernel-only access */
#define NEOGPU_MEM_USER_MMAP		BIT(1)  /* Mappable to user space */
#define NEOGPU_MEM_CPU_ACCESS		BIT(2)  /* CPU accessible via BAR1 */
#define NEOGPU_MEM_CE_TRANSFER		BIT(3)  /* Use Copy Engine for CPU access */

/**
 * struct neogpu_mem_block - Buddy allocator block tracking
 * @start: Start page frame number (in units of PAGE_SIZE)
 * @order: Block size order (2^order pages)
 * @free: Whether block is free
 * @list: Link to buddy list
 */
struct neogpu_mem_block {
	unsigned long start;
	unsigned int order;
	bool free;
	struct list_head list;
};

/**
 * struct neogpu_mem_mgr - VRAM Buddy Allocator Manager
 * @gpu: Back pointer to device
 * @lock: Protects allocator structures
 * @total_pages: Total 4KB pages in VRAM (4GB / 4KB = 1M pages)
 * @free_pages: Currently free pages
 * @free_list: Free lists per order (buddy system)
 * @bitmap: Optional bitmap for tracking (simpler than pure buddy)
 * @base_addr: Physical base address of VRAM (BAR1 offset 0)
 */
struct neogpu_mem_mgr {
	struct neogpu_device *gpu;
	spinlock_t lock;
	
	unsigned long total_pages;
	unsigned long free_pages;
	
	/* Buddy system free lists */
	struct list_head free_list[NEOGPU_NUM_BUCKETS];
	
	/* Simple allocation tracking (bitmap per page) 
	 * For 4GB/4KB = 1M pages = 1M bits = 128KB bitmap */
	unsigned long *page_bitmap;
	
	/* Statistics */
	u64 bytes_allocated;
	u64 bytes_freed;
	u32 allocs_failed;
};

/**
 * struct neogpu_bo - Buffer Object (GEM-like)
 * @gpu: Device pointer
 * @gpu_addr: GPU physical address (BAR1 offset or HBM addr)
 * @size: Size in bytes
 * @num_pages: Size in pages
 * @cpu_ptr: CPU mapping (via BAR1 window or vmap)
 * @page_offset: Offset in BAR1 (for direct mmap)
 * @refcount: Reference count
 * @flags: Allocation flags
 * @list: Link to bo list
 * @lock: Protects BO state
 * 
 * Note: CPU access via BAR1 is limited to mapped window size.
 * For full 4GB access, use Copy Engine.
 */
struct neogpu_bo {
	struct neogpu_device *gpu;
	u64 gpu_addr;
	size_t size;
	unsigned int num_pages;
	
	/* CPU access */
	void __iomem *cpu_ptr;		/* iomap if within BAR1 window */
	void *vmap_ptr;			/* vmalloc for staging buffer (CE mode) */
	
	/* BAR1 window info */
	resource_size_t bar1_offset;	/* Offset in BAR1 */
	bool bar1_mapped;		/* Directly mapped in BAR1 window */
	
	/* Metadata */
	refcount_t refcount;
	u32 flags;
	struct list_head list;
	spinlock_t lock;
	
	/* Sync */
	struct dma_fence *fence;	/* Completion fence */
	bool busy;			/* Currently in use by GPU */
};

/* Memory Manager Lifecycle */
int neogpu_mem_mgr_init(struct neogpu_device *gpu);
void neogpu_mem_mgr_fini(struct neogpu_device *gpu);

/* Buffer Object Allocation/Free */
struct neogpu_bo *neogpu_bo_alloc(struct neogpu_device *gpu, size_t size, u32 flags);
void neogpu_bo_free(struct neogpu_bo *bo);
struct neogpu_bo *neogpu_bo_ref(struct neogpu_bo *bo);
void neogpu_bo_unref(struct neogpu_bo *bo);

/* CPU Access APIs */
int neogpu_bo_kmap(struct neogpu_bo *bo);		/* Map to kernel space */
void neogpu_bo_kunmap(struct neogpu_bo *bo);
int neogpu_bo_mmap(struct neogpu_bo *bo, struct vm_area_struct *vma); /* User mmap */

/* Data Transfer (CPU <-> GPU) */
int neogpu_bo_memcpy_to_gpu(struct neogpu_bo *bo, const void *src, size_t len, loff_t offset);
int neogpu_bo_memcpy_from_gpu(struct neogpu_bo *bo, void *dst, size_t len, loff_t offset);
int neogpu_bo_clear(struct neogpu_bo *bo, int value, size_t len, loff_t offset);

/* Copy Engine Async Transfer (placeholder for Phase 4) */
int neogpu_bo_ce_upload(struct neogpu_bo *bo, struct sg_table *sgt, size_t len);
int neogpu_bo_ce_download(struct neogpu_bo *bo, struct sg_table *sgt, size_t len);

/* Debug */
void neogpu_mem_stats(struct neogpu_device *gpu);
void neogpu_dump_bo_list(struct neogpu_device *gpu);

#endif /* _NEOGPU_MEM_H_ */