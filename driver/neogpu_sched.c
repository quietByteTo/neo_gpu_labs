// SPDX-License-Identifier: GPL-2.0-only
/*
 * neogpu_sched.c - NeoGPU Command Scheduler
 * 
 * Manages ring buffer submission to RTL giga_thread_scheduler.v via Doorbell.
 * Supports multiple queues (Compute, Copy) and per-process contexts.
 */

#include <linux/slab.h>
#include <linux/dma-fence.h>
#include <linux/sched.h>
#include <linux/wait.h>
#include <linux/jiffies.h>

#include "neogpu_sched.h"
#include "neogpu_drv.h"
#include "neogpu_hw.h"
#include "neogpu_mem.h"

/* Global context ID allocator */
static atomic_t neogpu_ctx_id_counter = ATOMIC_INIT(0);

/**
 * neogpu_queue_init - Initialize a command queue
 * @queue: Queue structure to init
 * @gpu: Device
 * @type: Queue type (Compute or Copy)
 * @size: Ring buffer size (number of commands)
 */
int neogpu_queue_init(struct neogpu_queue *queue, struct neogpu_device *gpu,
		      enum neogpu_queue_type type, u32 size)
{
	size_t ring_size = size * sizeof(struct neogpu_command);
	int ret;
	
	memset(queue, 0, sizeof(*queue));
	queue->type = type;
	queue->gpu = gpu;
	queue->size = size;
	spin_lock_init(&queue->lock);
	init_waitqueue_head(&queue->wait_queue);
	queue->fence_context = dma_fence_context_alloc(1);
	
	/* Allocate ring buffer in VRAM (GPU must read it) */
	queue->ring_bo = neogpu_bo_alloc(gpu, ring_size, 
					   NEOGPU_MEM_KERNEL | NEOGPU_MEM_CPU_ACCESS);
	if (IS_ERR(queue->ring_bo)) {
		ret = PTR_ERR(queue->ring_bo);
		return ret;
	}
	
	ret = neogpu_bo_kmap(queue->ring_bo);
	if (ret) {
		neogpu_bo_unref(queue->ring_bo);
		return ret;
	}
	
	queue->ring_base = (struct neogpu_command *)queue->ring_bo->cpu_ptr;
	queue->ring_gpu_addr = queue->ring_bo->gpu_addr;
	
	/* Clear ring */
	memset(queue->ring_base, 0, ring_size);
	
	/* Select doorbell register based on type */
	if (type == NEOGPU_QUEUE_COPY) {
		queue->doorbell_reg = GPU_REG_CE_DESC_TAIL; /* Use CE for copies */
	} else {
		queue->doorbell_reg = GPU_REG_DOORBELL; /* Compute doorbell */
	}
	
	return 0;
}

/**
 * neogpu_queue_fini - Cleanup queue
 */
void neogpu_queue_fini(struct neogpu_queue *queue)
{
	if (!queue->ring_bo)
		return;
	
	/* Wait for idle */
	neogpu_wait_queue_idle(queue, 5000);
	
	neogpu_bo_kunmap(queue->ring_bo);
	neogpu_bo_unref(queue->ring_bo);
	queue->ring_bo = NULL;
}

/**
 * neogpu_ring_doorbell - Ring doorbell to notify GPU of new work
 * @queue: Queue to signal
 * 
 * Writes to GPU_REG_DOORBELL or GPU_REG_CE_DESC_TAIL to trigger
 * RTL giga_thread_scheduler.v or copy_engine.v
 */
void neogpu_ring_doorbell(struct neogpu_queue *queue)
{
	struct neogpu_device *gpu = queue->gpu;
	u32 write_idx;
	
	spin_lock(&queue->lock);
	write_idx = queue->write_idx;
	spin_unlock(&queue->lock);
	
	/* Write index to doorbell register */
	neogpu_write_reg32(gpu, queue->doorbell_reg, write_idx);
	
	dev_dbg(gpu->dev, "Queue %d doorbell: write_idx=%u\n", queue->type, write_idx);
}

/**
 * neogpu_wait_queue_idle - Wait until all commands complete
 * @queue: Queue to wait on
 * @timeout_ms: Timeout
 */
int neogpu_wait_queue_idle(struct neogpu_queue *queue, u32 timeout_ms)
{
	unsigned long timeout = jiffies + msecs_to_jiffies(timeout_ms);
	
	while (time_before(jiffies, timeout)) {
		u32 pending;
		
		spin_lock_irq(&queue->lock);
		pending = (queue->write_idx - queue->read_idx) & (queue->size - 1);
		spin_unlock_irq(&queue->lock);
		
		if (pending == 0)
			return 0;
		
		msleep(1);
	}
	
	return -ETIME;
}

/**
 * neogpu_submit_kernel - Submit kernel launch command
 * @ctx: GPU context
 * @kernel_addr: GPU address of kernel microcode
 * @grid: Grid dimensions [x,y,z]
 * @block: Block dimensions [x,y,z]
 * @args_ptr: GPU address of kernel arguments
 * @shared_mem: Dynamic shared memory size
 * @fence: Optional output fence
 * 
 * Packs command into ring buffer and rings doorbell.
 * Corresponds to RTL giga_thread_scheduler.v Grid dispatch.
 */
int neogpu_submit_kernel(struct neogpu_context *ctx,
			 u64 kernel_addr, u32 grid[3], u32 block[3],
			 u64 args_ptr, u64 shared_mem,
			 struct dma_fence **fence)
{
	struct neogpu_queue *queue = &ctx->queues[NEOGPU_QUEUE_COMPUTE];
	struct neogpu_device *gpu = ctx->gpu;
	struct neogpu_command *cmd;
	unsigned long flags;
	u32 slot;
	u32 next_idx;
	
	if (!ctx->vm) {
		dev_err(gpu->dev, "Context has no VM\n");
		return -EINVAL;
	}
	
	spin_lock_irqsave(&queue->lock, flags);
	
	/* Check if ring is full (leave 1 slot gap) */
	next_idx = (queue->write_idx + 1) & (queue->size - 1);
	if (next_idx == queue->read_idx) {
		spin_unlock_irqrestore(&queue->lock, flags);
		return -EBUSY; /* Ring full */
	}
	
	slot = queue->write_idx;
	cmd = &queue->ring_base[slot];
	
	/* Fill command */
	cmd->header.type = NEOGPU_CMD_KERNEL_LAUNCH;
	cmd->header.flags = 0x1; /* Request completion IRQ */
	cmd->header.seqno = ++queue->last_seqno;
	
	cmd->payload.kernel.kernel_addr = kernel_addr;
	cmd->payload.kernel.grid_dim[0] = grid[0];
	cmd->payload.kernel.grid_dim[1] = grid[1];
	cmd->payload.kernel.grid_dim[2] = grid[2];
	cmd->payload.kernel.block_dim[0] = block[0];
	cmd->payload.kernel.block_dim[1] = block[1];
	cmd->payload.kernel.block_dim[2] = block[2];
	cmd->payload.kernel.args_ptr = args_ptr;
	cmd->payload.kernel.shared_mem_size = shared_mem;
	
	/* Advance write pointer */
	queue->write_idx = next_idx;
	queue->pending++;
	
	spin_unlock_irqrestore(&queue->lock, flags);
	
	/* Ring doorbell to wake GPU */
	neogpu_ring_doorbell(queue);
	
	ctx->submit_count++;
	
	/* Create fence if requested */
	if (fence) {
		struct dma_fence *f = kzalloc(sizeof(*f), GFP_KERNEL);
		if (f) {
			dma_fence_init(f, &neogpu_fence_ops, &queue->lock,
				       queue->fence_context, cmd->header.seqno);
			*fence = f;
		}
	}
	
	dev_dbg(gpu->dev, "Submitted kernel @0x%llx, grid=%ux%ux%u, seqno=%llu\n",
		kernel_addr, grid[0], grid[1], grid[2], cmd->header.seqno);
	
	return 0;
}

/**
 * neogpu_submit_barrier - Insert execution barrier
 * @ctx: Context
 * @scope: 0=CTA, 1=Grid, 2=System
 */
int neogpu_submit_barrier(struct neogpu_context *ctx, u32 scope)
{
	struct neogpu_queue *queue = &ctx->queues[NEOGPU_QUEUE_COMPUTE];
	struct neogpu_command *cmd;
	unsigned long flags;
	u32 slot;
	
	spin_lock_irqsave(&queue->lock, flags);
	
	slot = queue->write_idx;
	cmd = &queue->ring_base[slot];
	
	cmd->header.type = NEOGPU_CMD_BARRIER;
	cmd->header.flags = 0;
	cmd->header.seqno = ++queue->last_seqno;
	cmd->payload.barrier.scope = scope;
	
	queue->write_idx = (queue->write_idx + 1) & (queue->size - 1);
	
	spin_unlock_irqrestore(&queue->lock, flags);
	
	neogpu_ring_doorbell(queue);
	return 0;
}

/* Fence ops placeholder */
static const struct dma_fence_ops neogpu_fence_ops = {
	.get_driver_name = NULL, /* TODO */
};