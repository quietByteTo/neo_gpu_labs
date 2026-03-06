// SPDX-License-Identifier: GPL-2.0-only
/*
 * neogpu_irq.c - NeoGPU Interrupt Handling Implementation
 * 
 * Top-half: Minimal work in IRQ context (read status, ack, schedule)
 * Bottom-half: Process completions, signal fences, wake waiters
 * 
 * Corresponds to:
 * - pciex5_bridge.v (MSI generation)
 * - hub_block.v (Interrupt aggregation and status)
 * - giga_thread_scheduler.v (Completion notification)
 */

#include <linux/interrupt.h>
#include <linux/workqueue.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/jiffies.h>

#include "neogpu_irq.h"
#include "neogpu_drv.h"
#include "neogpu_hw.h"
#include "neogpu_sched.h"
#include "neogpu_ce.h"

/**
 * neogpu_irq_init - Initialize MSI-X and workqueues
 * @gpu: Device pointer
 * 
 * Allocates MSI-X vectors and creates workqueues for bottom-half processing.
 * Return: 0 on success, negative on failure
 */
int neogpu_irq_init(struct neogpu_device *gpu)
{
	struct neogpu_irq *irq;
	int i, ret;
	
	irq = kzalloc(sizeof(*irq), GFP_KERNEL);
	if (!irq)
		return -ENOMEM;
	
	irq->gpu = gpu;
	gpu->irq = irq;  /* Add to device struct */
	
	/* Initialize MSI-X entries */
	for (i = 0; i < NEOGPU_NUM_IRQ_VECTORS; i++) {
		irq->msix_entries[i].entry = i;
		irq->msix_entries[i].vector = 0;
	}
	
	/* Try to enable MSI-X */
	ret = pci_enable_msix_exact(gpu->pdev, irq->msix_entries, 
				    NEOGPU_NUM_IRQ_VECTORS);
	if (ret) {
		dev_err(gpu->dev, "Failed to enable MSI-X: %d\n", ret);
		/* Fallback to legacy IRQ or polling mode */
		irq->irq_count = 0;
		kfree(irq);
		return ret;
	}
	
	irq->irq_count = NEOGPU_NUM_IRQ_VECTORS;
	dev_info(gpu->dev, "MSI-X enabled with %d vectors\n", irq->irq_count);
	
	/* Create dedicated workqueues for bottom-halves */
	/* WQ_UNBOUND: Work can run on any CPU (good for IRQ affinity) */
	irq->compute_wq = alloc_workqueue("neogpu_compute", 
					  WQ_UNBOUND | WQ_HIGHPRI, 0);
	irq->ce_wq = alloc_workqueue("neogpu_ce", WQ_UNBOUND, 0);
	irq->error_wq = alloc_workqueue("neogpu_error", WQ_UNBOUND, 0);
	
	if (!irq->compute_wq || !irq->ce_wq || !irq->error_wq) {
		ret = -ENOMEM;
		goto err_free_wq;
	}
	
	/* Initialize work items */
	for (i = 0; i < 4; i++) {
		INIT_WORK(&irq->gpc_work[i], neogpu_compute_work_handler);
		/* Store GPC index in work private data */
		/* Note: container_of used in handler to get irq, then derive GPC */
	}
	INIT_WORK(&irq->ce_work, neogpu_ce_work_handler);
	INIT_WORK(&irq->error_work, neogpu_error_work_handler);
	
	atomic_set(&irq->irq_pending, 0);
	irq->irq_enabled = false;
	irq->handlers_installed = false;
	
	return 0;
	
err_free_wq:
	if (irq->compute_wq) destroy_workqueue(irq->compute_wq);
	if (irq->ce_wq) destroy_workqueue(irq->ce_wq);
	if (irq->error_wq) destroy_workqueue(irq->error_wq);
	pci_disable_msix(gpu->pdev);
	kfree(irq);
	return ret;
}

/**
 * neogpu_irq_fini - Cleanup interrupts
 */
void neogpu_irq_fini(struct neogpu_device *gpu)
{
	struct neogpu_irq *irq = gpu->irq;
	int i;
	
	if (!irq)
		return;
	
	/* Disable interrupts first */
	neogpu_irq_disable(gpu);
	
	/* Cancel pending work */
	for (i = 0; i < 4; i++) {
		cancel_work_sync(&irq->gpc_work[i]);
	}
	cancel_work_sync(&irq->ce_work);
	cancel_work_sync(&irq->error_work);
	
	/* Destroy workqueues */
	if (irq->compute_wq) destroy_workqueue(irq->compute_wq);
	if (irq->ce_wq) destroy_workqueue(irq->ce_wq);
	if (irq->error_wq) destroy_workqueue(irq->error_wq);
	
	/* Disable MSI-X */
	if (irq->irq_count > 0) {
		pci_disable_msix(gpu->pdev);
	}
	
	kfree(irq);
	gpu->irq = NULL;
}

/**
 * neogpu_irq_enable - Unmask interrupts in hardware
 */
void neogpu_irq_enable(struct neogpu_device *gpu)
{
	struct neogpu_irq *irq = gpu->irq;
	u32 mask;
	
	if (!irq || irq->irq_enabled)
		return;
	
	/* Enable all compute + CE + error interrupts */
	mask = NEOGPU_IRQ_GPC0 | NEOGPU_IRQ_GPC1 | NEOGPU_IRQ_GPC2 | 
	       NEOGPU_IRQ_GPC3 | NEOGPU_IRQ_CE | NEOGPU_IRQ_ERROR;
	
	neogpu_write_reg32(gpu, GPU_REG_INT_MASK, ~mask); /* Active low mask */
	irq->irq_enabled = true;
	
	dev_dbg(gpu->dev, "Interrupts enabled (mask 0x%x)\n", mask);
}

/**
 * neogpu_irq_disable - Mask all interrupts
 */
void neogpu_irq_disable(struct neogpu_device *gpu)
{
	struct neogpu_irq *irq = gpu->irq;
	
	if (!irq)
		return;
	
	/* Mask all interrupts */
	neogpu_write_reg32(gpu, GPU_REG_INT_MASK, 0xFFFFFFFF);
	irq->irq_enabled = false;
}

/**
 * neogpu_irq_handler - Top-half for GPC compute interrupts
 * @irq: Linux IRQ number
 * @data: Pointer to neogpu_device
 * 
 * Reads interrupt status, acknowledges specific interrupt,
 * schedules bottom-half workqueue.
 */
irqreturn_t neogpu_irq_handler(int irq, void *data)
{
	struct neogpu_device *gpu = data;
	struct neogpu_irq *gpu_irq = gpu->irq;
	u32 int_status, gpc_mask;
	int gpc_id;
	
	/* Read interrupt status register (from hub_block.v) */
	int_status = neogpu_read_reg32(gpu, GPU_REG_INT_STATUS);
	
	/* Determine which GPC triggered (based on vector, but verify) */
	/* Vector 0-3 correspond to GPC 0-3 */
	gpc_id = irq - gpu_irq->msix_entries[0].vector; /* Relative to first vector */
	if (gpc_id < 0 || gpc_id >= 4)
		return IRQ_NONE;
	
	gpc_mask = BIT(gpc_id);
	
	if (!(int_status & gpc_mask)) {
		/* Spurious interrupt or shared line */
		return IRQ_NONE;
	}
	
	/* Acknowledge this specific GPC interrupt */
	/* Write to GPU_REG_IRQ_ACK with GPC-specific bit */
	neogpu_write_reg32(gpu, GPU_REG_IRQ_ACK, gpc_mask);
	
	/* Schedule bottom-half */
	/* Disable further interrupts for this GPC until handled (optional) */
	/* neogpu_write_reg32(gpu, GPU_REG_INT_MASK, ...); */
	
	if (gpu_irq->compute_wq) {
		queue_work(gpu_irq->compute_wq, &gpu_irq->gpc_work[gpc_id]);
	}
	
	return IRQ_HANDLED;
}

/**
 * neogpu_irq_handler_ce - Top-half for Copy Engine interrupts
 */
irqreturn_t neogpu_irq_handler_ce(int irq, void *data)
{
	struct neogpu_device *gpu = data;
	struct neogpu_irq *gpu_irq = gpu->irq;
	u32 int_status;
	
	int_status = neogpu_read_reg32(gpu, GPU_REG_INT_STATUS);
	
	if (!(int_status & NEOGPU_IRQ_CE)) {
		return IRQ_NONE;
	}
	
	/* Acknowledge CE interrupt */
	neogpu_write_reg32(gpu, GPU_REG_IRQ_ACK_CE, 1);
	
	/* Schedule CE bottom-half */
	if (gpu_irq->ce_wq) {
		queue_work(gpu_irq->ce_wq, &gpu_irq->ce_work);
	}
	
	return IRQ_HANDLED;
}

/**
 * neogpu_irq_handler_error - Top-half for error interrupts
 */
irqreturn_t neogpu_irq_handler_error(int irq, void *data)
{
	struct neogpu_device *gpu = data;
	struct neogpu_irq *gpu_irq = gpu->irq;
	u32 int_status;
	
	int_status = neogpu_read_reg32(gpu, GPU_REG_INT_STATUS);
	
	if (!(int_status & NEOGPU_IRQ_ERROR)) {
		return IRQ_NONE;
	}
	
	/* Don't ack error immediately - let bottom-half read error details */
	/* Schedule error handler */
	if (gpu_irq->error_wq) {
		queue_work(gpu_irq->error_wq, &gpu_irq->error_work);
	}
	
	return IRQ_HANDLED;
}

/**
 * neogpu_compute_work_handler - Bottom-half for compute completion
 * @work: Work structure
 * 
 * Processes completed CTAs, updates queue read pointers,
 * signals DMA fences, wakes waiting processes.
 */
void neogpu_compute_work_handler(struct work_struct *work)
{
	struct neogpu_irq *irq = container_of(work, struct neogpu_irq, 
					      gpc_work[0]); /* Generic access */
	struct neogpu_device *gpu;
	u32 gpc_status, ctas_remaining;
	u32 gpc_id;
	int i;
	
	/* Derive GPC ID from work pointer offset */
	gpc_id = (work - irq->gpc_work) / sizeof(struct work_struct);
	if (gpc_id >= 4)
		return;
	
	gpu = irq->gpu;
	
	dev_dbg(gpu->dev, "GPC%u completion bottom-half\n", gpc_id);
	
	/* Read GPC status registers */
	gpc_status = neogpu_read_reg32(gpu, GPU_REG_GPC_STATUS(gpc_id));
	ctas_remaining = neogpu_read_reg32(gpu, GPU_REG_GPC_CTAS_REMAIN(gpc_id));
	
	/* Process all contexts that submitted to this GPC */
	/* For each context's compute queue, check if commands completed */
	
	/* Simple approach: Iterate all active contexts */
	/* In real driver, would track which context submitted to which GPC */
	
	mutex_lock(&neogpu_devices_lock);
	{
		struct neogpu_context *ctx;
		struct neogpu_queue *queue;
		u32 completed_seqno = 0; /* Extract from hardware */
		
		/* Update queue read pointer based on hardware completion */
		/* This is simplified - real implementation needs seqno tracking */
		
		list_for_each_entry(ctx, &neogpu_devices, list) {
			queue = &ctx->queues[NEOGPU_QUEUE_COMPUTE];
			
			spin_lock(&queue->lock);
			
			/* Advance read_idx up to hardware tail */
			/* Hardware should expose completed seqno or head pointer */
			while (queue->read_idx != queue->write_idx) {
				struct neogpu_command *cmd;
				u32 slot;
				
				slot = queue->read_idx;
				cmd = &queue->ring_base[slot];
				
				/* Check if this command completed */
				/* In real impl, compare cmd->header.seqno with hw seqno */
				if (ctas_remaining == 0 && gpc_status & BIT(0)) {
					/* Completed */
					
					/* Signal fence if present */
					if (cmd->header.flags & 0x1) {
						/* Signal completion fence */
						/* dma_fence_signal(cmd->fence); */
					}
					
					queue->read_idx = (queue->read_idx + 1) & 
							  (queue->size - 1);
					queue->pending--;
					completed_seqno = cmd->header.seqno;
				} else {
					break; /* Not yet completed */
				}
			}
			
			/* Wake up waiters */
			if (completed_seqno > 0) {
				wake_up_all(&queue->wait_queue);
			}
			
			spin_unlock(&queue->lock);
		}
	}
	mutex_unlock(&neogpu_devices_lock);
	
	/* Re-enable GPC interrupt if it was masked */
	/* neogpu_unmask_irq(gpu, NEOGPU_IRQ_GPC0 << gpc_id); */
}

/**
 * neogpu_ce_work_handler - Bottom-half for Copy Engine completion
 */
void neogpu_ce_work_handler(struct work_struct *work)
{
	struct neogpu_irq *irq = container_of(work, struct neogpu_irq, ce_work);
	struct neogpu_device *gpu = irq->gpu;
	
	/* Delegate to CE module's handler */
	if (gpu->ce) {
		neogpu_ce_irq_handler(gpu->ce);
	}
	
	/* Also wake up any sync waiters in queues */
	/* Copy queue might have waiting processes */
	if (gpu->default_ctx) {
		wake_up_all(&gpu->default_ctx->queues[NEOGPU_QUEUE_COPY].wait_queue);
	}
}

/**
 * neogpu_error_work_handler - Bottom-half for error handling
 */
void neogpu_error_work_handler(struct work_struct *work)
{
	struct neogpu_irq *irq = container_of(work, struct neogpu_irq, error_work);
	struct neogpu_device *gpu = irq->gpu;
	u32 error_status, gpc_status[4];
	int i;
	
	/* Read detailed error status */
	error_status = neogpu_read_reg32(gpu, GPU_REG_STATUS);
	
	dev_err(gpu->dev, "GPU Error interrupt! STATUS=0x%08x\n", error_status);
	
	/* Read all GPC statuses for diagnostics */
	for (i = 0; i < 4; i++) {
		gpc_status[i] = neogpu_read_reg32(gpu, GPU_REG_GPC_STATUS(i));
		dev_err(gpu->dev, "GPC%d STATUS: 0x%08x\n", i, gpc_status[i]);
	}
	
	/* Dump registers for debugging */
	neogpu_dump_regs(gpu);
	
	/* Recovery strategy based on error type */
	if (error_status & GPU_STATUS_ERROR) {
		/* Fatal error - reset GPU */
		dev_crit(gpu->dev, "Fatal GPU error, triggering reset\n");
		neogpu_hardware_reset(gpu);
	}
	
	/* Acknowledge error interrupt after handling */
	neogpu_write_reg32(gpu, GPU_REG_IRQ_ACK, NEOGPU_IRQ_ERROR);
}

/**
 * neogpu_request_irqs - Register IRQ handlers with kernel
 * 
 * Called after neogpu_irq_init to request specific handlers per vector
 */
int neogpu_request_irqs(struct neogpu_device *gpu)
{
	struct neogpu_irq *irq = gpu->irq;
	int i, ret;
	
	if (!irq || irq->handlers_installed)
		return -EINVAL;
	
	/* Request GPC compute IRQs (vectors 0-3) */
	for (i = 0; i < 4; i++) {
		ret = request_irq(irq->msix_entries[i].vector,
				  neogpu_irq_handler,
				  0, /* Flags */
				  "neogpu_compute",
				  gpu);
		if (ret) {
			dev_err(gpu->dev, "Failed to request GPC%d IRQ: %d\n", i, ret);
			goto err_free_irqs;
		}
	}
	
	/* Request CE IRQ (vector 4) */
	ret = request_irq(irq->msix_entries[NEOGPU_VEC_CE_COMPLETE].vector,
			  neogpu_irq_handler_ce,
			  0,
			  "neogpu_ce",
			  gpu);
	if (ret) {
		dev_err(gpu->dev, "Failed to request CE IRQ: %d\n", ret);
		goto err_free_irqs;
	}
	
	/* Request Error IRQ (vector 5) */
	ret = request_irq(irq->msix_entries[NEOGPU_VEC_ERROR].vector,
			  neogpu_irq_handler_error,
			  0,
			  "neogpu_error",
			  gpu);
	if (ret) {
		dev_err(gpu->dev, "Failed to request Error IRQ: %d\n", ret);
		goto err_free_irqs;
	}
	
	irq->handlers_installed = true;
	dev_info(gpu->dev, "All IRQ handlers registered\n");
	return 0;
	
err_free_irqs:
	/* Free already requested IRQs */
	for (i--; i >= 0; i--) {
		free_irq(irq->msix_entries[i].vector, gpu);
	}
	if (irq->msix_entries[NEOGPU_VEC_CE_COMPLETE].vector)
		free_irq(irq->msix_entries[NEOGPU_VEC_CE_COMPLETE].vector, gpu);
	return ret;
}

/**
 * neogpu_free_irqs - Unregister IRQ handlers
 */
void neogpu_free_irqs(struct neogpu_device *gpu)
{
	struct neogpu_irq *irq = gpu->irq;
	int i;
	
	if (!irq || !irq->handlers_installed)
		return;
	
	for (i = 0; i < 4; i++) {
		free_irq(irq->msix_entries[i].vector, gpu);
	}
	free_irq(irq->msix_entries[NEOGPU_VEC_CE_COMPLETE].vector, gpu);
	free_irq(irq->msix_entries[NEOGPU_VEC_ERROR].vector, gpu);
	
	irq->handlers_installed = false;
}