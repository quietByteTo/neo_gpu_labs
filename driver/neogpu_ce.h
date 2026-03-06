/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * neogpu_ce.h - NeoGPU Copy Engine (DMA) Interface
 * 
 * Async DMA transfer between Host memory and GPU VRAM (H2D/D2H)
 * Descriptor ring management and DMA Fence synchronization.
 */

#ifndef _NEOGPU_CE_H_
#define _NEOGPU_CE_H_

#include <linux/types.h>
#include <linux/dma-fence.h>
#include <linux/dma-fence-array.h>
#include <linux/dma-mapping.h>
#include <linux/scatterlist.h>
#include <linux/completion.h>
#include <linux/spinlock.h>

#include "neogpu_mem.h"

/* CE Descriptor Format (matches RTL copy_engine.v, 32 bytes) */
struct ce_desc {
	u64 src_addr;		/* Source address (Host PA or GPU PA) */
	u64 dst_addr;		/* Destination address (GPU PA or Host PA) */
	u32 length;		/* Transfer length in bytes */
	u32 control;		/* Control flags */
	u64 next_desc;		/* Next descriptor GPU address (0 if end) */
} __packed;

/* Control flags */
#define CE_CTRL_H2D		0x00	/* Host to Device (bit 0 = 0) */
#define CE_CTRL_D2H		0x01	/* Device to Host (bit 0 = 1) */
#define CE_CTRL_IRQ_EN		0x02	/* Interrupt on completion (bit 1) */
#define CE_CTRL_LAST		0x04	/* Last descriptor in chain (bit 2) */

/* Descriptor Ring Configuration */
#define CE_RING_SIZE		256	/* Number of descriptors in ring */
#define CE_RING_SIZE_BYTES	(CE_RING_SIZE * sizeof(struct ce_desc))

/**
 * struct neogpu_ce - Copy Engine State
 * @gpu: Back pointer to device
 * @desc_bo: Buffer object for descriptor ring (GPU accessible)
 * @desc_ring: CPU virtual address of descriptor ring (BAR1 mapped or vmap)
 * @ring_base_gpu: GPU physical address of ring base
 * 
 * @head: Driver write pointer (next free slot)
 * @tail: Hardware read pointer (completed)
 * @free_slots: Available descriptor slots
 * 
 * @lock: Protects ring manipulation
 * @fence_context: DMA Fence context ID
 * @fence_seqno: Sequence number for fences
 * 
 * @pending_fences: Array of pending fences indexed by ring slot
 * @completion: For synchronous operations
 * @irq_enabled: Whether CE IRQ is enabled
 */
struct neogpu_ce {
	struct neogpu_device *gpu;
	struct neogpu_bo *desc_bo;		/* VRAM for descriptors */
	struct ce_desc *desc_ring;		/* CPU mapping of ring */
	u64 ring_base_gpu;			/* GPU address of ring */
	
	u32 head;				/* Driver produces */
	u32 tail;				/* HW consumes (RO from reg) */
	u32 free_slots;
	
	spinlock_t lock;			/* Ring lock */
	u64 fence_context;			/* Fence context */
	u32 fence_seqno;			/* Incrementing seqno */
	
	/* Pending operations tracking */
	struct dma_fence **pending_fences;	/* Fence per slot */
	struct completion *pending_completions;	/* Optional completion per slot */
	
	bool irq_enabled;
	u32 bytes_transferred;			/* Stats */
};

/* Lifecycle */
int neogpu_ce_init(struct neogpu_device *gpu);
void neogpu_ce_fini(struct neogpu_device *gpu);

/* Async Transfer (returns immediately with fence) */
int neogpu_copy_async(struct neogpu_device *gpu,
		      u64 src_gpu_addr, u64 dst_gpu_addr, size_t len,
		      enum dma_data_direction dir, /* DMA_TO_DEVICE/DMA_FROM_DEVICE */
		      struct dma_fence **fence_out);

/* Sync Transfer (blocks until complete) */
int neogpu_copy_sync(struct neogpu_device *gpu,
		     u64 src_gpu_addr, u64 dst_gpu_addr, size_t len,
		     enum dma_data_direction dir,
		     unsigned long timeout_ms);

/* Scatter-Gather Async (multiple buffers) */
int neogpu_copy_sg_async(struct neogpu_device *gpu,
			 struct sg_table *src_sgt, u64 dst_gpu_addr,
			 enum dma_data_direction dir,
			 struct dma_fence **fence_out);

/* Interrupt Handler (called from neogpu_irq.c) */
void neogpu_ce_irq_handler(struct neogpu_ce *ce);

/* Debug */
void neogpu_ce_dump_stats(struct neogpu_ce *ce);

#endif /* _NEOGPU_CE_H_ */