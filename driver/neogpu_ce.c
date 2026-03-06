// SPDX-License-Identifier: GPL-2.0-only
/*
 * neogpu_ce.c - NeoGPU Copy Engine Implementation
 * 
 * Manages descriptor ring in VRAM, submits DMA jobs to RTL copy_engine.v,
 * and implements DMA Fence synchronization for async operations.
 */

#include <linux/slab.h>
#include <linux/dma-mapping.h>
#include <linux/dma-fence.h>
#include <linux/completion.h>
#include <linux/jiffies.h>
#include <linux/io.h>

#include "neogpu_ce.h"
#include "neogpu_drv.h"
#include "neogpu_hw.h"

/* Internal fence implementation for CE */
struct ce_fence {
	struct dma_fence base;
	struct neogpu_ce *ce;
	u32 ring_slot;		/* Which descriptor slot this fence monitors */
};

static inline struct ce_fence *to_ce_fence(struct dma_fence *fence)
{
	return container_of(fence, struct ce_fence, base);
}

static const char *ce_fence_get_driver_name(struct dma_fence *fence)
{
	return "neogpu_ce";
}

static const char *ce_fence_get_timeline_name(struct dma_fence *fence)
{
	return "ce_ring";
}

static bool ce_fence_signaled(struct dma_fence *fence)
{
	struct ce_fence *cf = to_ce_fence(fence);
	struct neogpu_ce *ce = cf->ce;
	u32 tail;
	
	/* Check if tail has passed our slot */
	tail = neogpu_read_reg32(ce->gpu, GPU_REG_CE_DESC_HEAD);
	
	if (ce->head >= ce->tail) {
		/* Normal case: tail < slot <= head */
		return (tail >= cf->ring_slot) || (tail < ce->tail);
	} else {
		/* Wrap case: slot > head, tail wrapped */
		return (tail >= cf->ring_slot) && (tail < ce->tail);
	}
}

static void ce_fence_release(struct dma_fence *fence)
{
	struct ce_fence *cf = to_ce_fence(fence);
	
	kfree(cf);
}

static const struct dma_fence_ops ce_fence_ops = {
	.get_driver_name = ce_fence_get_driver_name,
	.get_timeline_name = ce_fence_get_timeline_name,
	.signaled = ce_fence_signaled,
	.release = ce_fence_release,
};

/**
 * ce_alloc_fence - Allocate a fence for a ring slot
 */
static struct dma_fence *ce_alloc_fence(struct neogpu_ce *ce, u32 slot)
{
	struct ce_fence *cf;
	
	cf = kzalloc(sizeof(*cf), GFP_KERNEL);
	if (!cf)
		return NULL;
	
	cf->ce = ce;
	cf->ring_slot = slot;
	
	dma_fence_init(&cf->base, &ce_fence_ops, &ce->lock, 
		       ce->fence_context, ce->fence_seqno++);
	
	return &cf->base;
}

/**
 * neogpu_ce_init - Initialize Copy Engine
 * @gpu: Device pointer
 * 
 * Allocates descriptor ring in VRAM, maps it for CPU access,
 * initializes hardware registers.
 * 
 * Return: 0 on success, -ENOMEM on failure
 */
int neogpu_ce_init(struct neogpu_device *gpu)
{
	struct neogpu_ce *ce;
	u64 ring_base;
	int ret;
	
	ce = kzalloc(sizeof(*ce), GFP_KERNEL);
	if (!ce)
		return -ENOMEM;
	
	ce->gpu = gpu;
	gpu->ce = ce;  /* Add to device struct */
	spin_lock_init(&ce->lock);
	ce->fence_context = dma_fence_context_alloc(1);
	ce->fence_seqno = 0;
	
	/* Allocate descriptor ring in VRAM (must be GPU accessible) */
	ce->desc_bo = neogpu_bo_alloc(gpu, CE_RING_SIZE_BYTES, 
				      NEOGPU_MEM_KERNEL | NEOGPU_MEM_CPU_ACCESS);
	if (IS_ERR(ce->desc_bo)) {
		ret = PTR_ERR(ce->desc_bo);
		dev_err(gpu->dev, "Failed to allocate CE descriptor ring: %d\n", ret);
		goto err_free_ce;
	}
	
	/* Map for CPU access (via BAR1 or staging) */
	ret = neogpu_bo_kmap(ce->desc_bo);
	if (ret) {
		dev_err(gpu->dev, "Failed to map CE ring: %d\n", ret);
		goto err_free_bo;
	}
	
	ce->desc_ring = (struct ce_desc *)ce->desc_bo->cpu_ptr;
	ce->ring_base_gpu = ce->desc_bo->gpu_addr;
	
	/* Clear ring */
	memset(ce->desc_ring, 0, CE_RING_SIZE_BYTES);
	
	/* Initialize tracking arrays */
	ce->pending_fences = kzalloc(CE_RING_SIZE * sizeof(struct dma_fence *), GFP_KERNEL);
	ce->pending_completions = kzalloc(CE_RING_SIZE * sizeof(struct completion), GFP_KERNEL);
	if (!ce->pending_fences || !ce->pending_completions) {
		ret = -ENOMEM;
		goto err_unmap;
	}
	
	/* Initialize completions */
	int i;
	for (i = 0; i < CE_RING_SIZE; i++) {
		init_completion(&ce->pending_completions[i]);
		/* Mark as "available" by completing immediately, then reinit when used */
		complete(&ce->pending_completions[i]);
	}
	
	ce->head = 0;
	ce->tail = 0;
	ce->free_slots = CE_RING_SIZE;
	
	/* Initialize hardware registers */
	neogpu_write_reg64(gpu, GPU_REG_CE_DESC_BASE_LO, ce->ring_base_gpu);
	neogpu_write_reg32(gpu, GPU_REG_CE_DESC_TAIL, 0); /* Reset tail */
	
	/* Enable CE with interrupts */
	neogpu_write_reg32(gpu, GPU_REG_CE_CTRL, CE_CTRL_ENABLE | CE_CTRL_IRQ_EN);
	ce->irq_enabled = true;
	
	dev_info(gpu->dev, "Copy Engine initialized: ring at GPU 0x%llx, %u slots\n",
		 ce->ring_base_gpu, CE_RING_SIZE);
	
	return 0;
	
err_unmap:
	neogpu_bo_kunmap(ce->desc_bo);
err_free_bo:
	neogpu_bo_unref(ce->desc_bo);
err_free_ce:
	kfree(ce);
	return ret;
}

/**
 * neogpu_ce_fini - Cleanup Copy Engine
 */
void neogpu_ce_fini(struct neogpu_device *gpu)
{
	struct neogpu_ce *ce = gpu->ce;
	int i;
	
	if (!ce)
		return;
	
	/* Wait for all operations to complete (or cancel) */
	spin_lock_irq(&ce->lock);
	ce->irq_enabled = false;
	spin_unlock_irq(&ce->lock);
	
	/* Disable CE */
	neogpu_write_reg32(gpu, GPU_REG_CE_CTRL, 0);
	
	/* Release pending fences */
	for (i = 0; i < CE_RING_SIZE; i++) {
		if (ce->pending_fences[i]) {
			dma_fence_put(ce->pending_fences[i]);
			ce->pending_fences[i] = NULL;
		}
	}
	
	kfree(ce->pending_fences);
	kfree(ce->pending_completions);
	
	neogpu_bo_kunmap(ce->desc_bo);
	neogpu_bo_unref(ce->desc_bo);
	kfree(ce);
	gpu->ce = NULL;
}

/**
 * ce_submit_descriptor - Submit single descriptor to ring
 * @ce: Copy Engine
 * @src: Source address
 * @dst: Destination address
 * @len: Length
 * @dir: Direction
 * @fence_out: Output fence
 * 
 * Internal function, must be called with ce->lock held
 * 
 * Return: 0 on success, -EBUSY if ring full
 */
static int ce_submit_descriptor(struct neogpu_ce *ce, u64 src, u64 dst, 
				u32 len, enum dma_data_direction dir,
				struct dma_fence **fence_out)
{
	struct ce_desc *desc;
	u32 slot;
	struct dma_fence *fence;
	
	if (ce->free_slots == 0)
		return -EBUSY;
	
	slot = ce->head;
	desc = &ce->desc_ring[slot];
	
	/* Fill descriptor (matches RTL format) */
	desc->src_addr = src;
	desc->dst_addr = dst;
	desc->length = len;
	desc->control = CE_CTRL_IRQ_EN; /* Always enable IRQ for tracking */
	
	if (dir == DMA_FROM_DEVICE)
		desc->control |= CE_CTRL_D2H; /* Device to Host */
	else
		desc->control |= CE_CTRL_H2D; /* Host to Device */
	
	/* Check if this is the last slot before wrap */
	if (((slot + 1) & (CE_RING_SIZE - 1)) == ce->tail)
		desc->control |= CE_CTRL_LAST; /* Mark last in chain */
	else
		desc->next_desc = ce->ring_base_gpu + ((slot + 1) * sizeof(struct ce_desc));
	
	/* Create fence for this operation */
	fence = ce_alloc_fence(ce, slot);
	if (!fence)
		return -ENOMEM;
	
	ce->pending_fences[slot] = fence;
	
	/* Reset completion for sync waiters */
	reinit_completion(&ce->pending_completions[slot]);
	
	/* Update head */
	ce->head = (ce->head + 1) & (CE_RING_SIZE - 1);
	ce->free_slots--;
	
	/* Write head to hardware (kicks CE) */
	neogpu_write_reg32(ce->gpu, GPU_REG_CE_DESC_TAIL, ce->head);
	
	if (fence_out)
		*fence_out = dma_fence_get(fence);
	else
		/* If no fence requested, we drop our reference but keep in array */
		dma_fence_put(fence);
	
	ce->bytes_transferred += len;
	
	return 0;
}

/**
 * neogpu_copy_async - Submit asynchronous DMA transfer
 * @gpu: Device
 * @src_gpu_addr: Source GPU address (or Host DMA address)
 * @dst_gpu_addr: Destination GPU address (or Host DMA address)
 * @len: Bytes to transfer
 * @dir: DMA_TO_DEVICE (H2D) or DMA_FROM_DEVICE (D2H)
 * @fence_out: Returned fence for completion tracking
 * 
 * Return: 0 on submit success, negative on error
 */
int neogpu_copy_async(struct neogpu_device *gpu,
		      u64 src_gpu_addr, u64 dst_gpu_addr, size_t len,
		      enum dma_data_direction dir,
		      struct dma_fence **fence_out)
{
	struct neogpu_ce *ce = gpu->ce;
	unsigned long flags;
	int ret;
	
	if (!ce)
		return -ENODEV;
	
	if (len > (1U << 30)) /* Max 1GB per descriptor */
		return -EINVAL;
	
	if (fence_out)
		*fence_out = NULL;
	
	spin_lock_irqsave(&ce->lock, flags);
	ret = ce_submit_descriptor(ce, src_gpu_addr, dst_gpu_addr, len, dir, fence_out);
	spin_unlock_irqrestore(&ce->lock, flags);
	
	return ret;
}

/**
 * neogpu_copy_sync - Synchronous DMA transfer (blocks until complete)
 * @gpu: Device
 * @src_gpu_addr: Source address
 * @dst_gpu_addr: Destination address  
 * @len: Bytes
 * @dir: Direction
 * @timeout_ms: Timeout in milliseconds
 * 
 * Return: 0 on success, -ETIME on timeout, -EIO on error
 */
int neogpu_copy_sync(struct neogpu_device *gpu,
		     u64 src_gpu_addr, u64 dst_gpu_addr, size_t len,
		     enum dma_data_direction dir,
		     unsigned long timeout_ms)
{
	struct neogpu_ce *ce = gpu->ce;
	struct dma_fence *fence;
	unsigned long flags;
	u32 slot;
	int ret;
	
	if (!ce)
		return -ENODEV;
	
	/* Submit asynchronously */
	spin_lock_irqsave(&ce->lock, flags);
	/* Calculate slot before submit (head will increment) */
	slot = ce->head;
	ret = ce_submit_descriptor(ce, src_gpu_addr, dst_gpu_addr, len, dir, &fence);
	spin_unlock_irqrestore(&ce->lock, flags);
	
	if (ret)
		return ret;
	
	/* Wait for completion using the completion object */
	ret = wait_for_completion_timeout(&ce->pending_completions[slot],
					  msecs_to_jiffies(timeout_ms));
	if (ret == 0) {
		dev_err(gpu->dev, "CE transfer timeout\n");
		return -ETIME;
	}
	
	/* Check fence status */
	if (dma_fence_get_status(fence) < 0) {
		ret = -EIO;
	} else {
		ret = 0;
	}
	
	dma_fence_put(fence);
	return ret;
}

/**
 * neogpu_copy_sg_async - Scatter-Gather DMA transfer
 * @gpu: Device
 * @src_sgt: Scatter-gather table (Host pages)
 * @dst_gpu_addr: Destination GPU address (must be contiguous)
 * @dir: Direction
 * @fence_out: Output fence for entire chain
 * 
 * Chains multiple descriptors for SG transfer.
 */
int neogpu_copy_sg_async(struct neogpu_device *gpu,
			 struct sg_table *src_sgt, u64 dst_gpu_addr,
			 enum dma_data_direction dir,
			 struct dma_fence **fence_out)
{
	struct neogpu_ce *ce = gpu->ce;
	struct scatterlist *sg;
	unsigned int i;
	unsigned long flags;
	u64 dst = dst_gpu_addr;
	struct dma_fence *fences[CE_RING_SIZE]; /* Temporary array */
	unsigned int nfences = 0;
	int ret = 0;
	
	if (!ce)
		return -ENODEV;
	
	spin_lock_irqsave(&ce->lock, flags);
	
	for_each_sg(src_sgt->sgl, sg, src_sgt->nents, i) {
		u64 src = sg_dma_address(sg);
		u32 len = sg_dma_len(sg);
		struct dma_fence *f;
		
		if (ce->free_slots == 0) {
			ret = -EBUSY;
			break;
		}
		
		ret = ce_submit_descriptor(ce, src, dst, len, dir, &f);
		if (ret)
			break;
		
		fences[nfences++] = f;
		dst += len;
	}
	
	spin_unlock_irqrestore(&ce->lock, flags);
	
	if (ret && nfences > 0) {
		/* Cancel submitted ones by waiting for them */
		for (i = 0; i < nfences; i++) {
			dma_fence_wait(fences[i], true);
			dma_fence_put(fences[i]);
		}
		return ret;
	}
	
	/* Return array fence if multiple, else single fence */
	if (fence_out && nfences > 0) {
		if (nfences == 1) {
			*fence_out = fences[0];
		} else {
			/* Create fence array */
			struct dma_fence_array *arr;
			arr = dma_fence_array_create(nfences, fences,
						     dma_fence_context_alloc(1),
						     0, false);
			if (arr)
				*fence_out = &arr->base;
			else
				*fence_out = fences[nfences - 1]; /* Fallback */
		}
	}
	
	return 0;
}

/**
 * neogpu_ce_irq_handler - CE completion interrupt handler
 * @ce: Copy Engine
 * 
 * Called from main IRQ handler when CE_IRQ detected.
 * Updates tail pointer and signals fences.
 */
void neogpu_ce_irq_handler(struct neogpu_ce *ce)
{
	struct neogpu_device *gpu = ce->gpu;
	u32 new_tail;
	unsigned long flags;
	
	spin_lock_irqsave(&ce->lock, flags);
	
	/* Read hardware tail (completion pointer) */
	new_tail = neogpu_read_reg32(gpu, GPU_REG_CE_DESC_HEAD);
	
	/* Process completed slots [ce->tail, new_tail) */
	while (ce->tail != new_tail) {
		u32 slot = ce->tail;
		struct dma_fence *fence = ce->pending_fences[slot];
		
		if (fence) {
			/* Signal fence */
			dma_fence_signal(fence);
			dma_fence_put(fence);
			ce->pending_fences[slot] = NULL;
		}
		
		/* Signal completion for sync waiters */
		complete(&ce->pending_completions[slot]);
		
		ce->tail = (ce->tail + 1) & (CE_RING_SIZE - 1);
		ce->free_slots++;
	}
	
	spin_unlock_irqrestore(&ce->lock, flags);
	
	/* Acknowledge interrupt */
	neogpu_write_reg32(gpu, GPU_REG_IRQ_ACK_CE, 1);
}

/**
 * neogpu_ce_dump_stats - Print CE statistics
 */
void neogpu_ce_dump_stats(struct neogpu_ce *ce)
{
	if (!ce)
		return;
	
	dev_info(ce->gpu->dev, "Copy Engine Stats:\n");
	dev_info(ce->gpu->dev, "  Ring: head=%u, tail=%u, free=%u/%u\n",
		 ce->head, ce->tail, ce->free_slots, CE_RING_SIZE);
	dev_info(ce->gpu->dev, "  Transferred: %u MB\n", 
		 ce->bytes_transferred >> 20);
	dev_info(ce->gpu->dev, "  Fence context: %llu, seqno: %u\n",
		 (unsigned long long)ce->fence_context, ce->fence_seqno);
}