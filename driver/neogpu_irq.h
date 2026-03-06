/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * neogpu_irq.h - NeoGPU Interrupt Management
 * 
 * MSI-X vector allocation, top-half/bottom-half interrupt handling,
 * and completion notification via DMA Fences.
 */

#ifndef _NEOGPU_IRQ_H_
#define _NEOGPU_IRQ_H_

#include <linux/interrupt.h>
#include <linux/workqueue.h>
#include <linux/irqreturn.h>

/* MSI-X Vector Allocation (8 vectors total) */
enum neogpu_irq_vector {
	NEOGPU_VEC_GPC0_COMPUTE = 0,	/* GPC 0 CTA completion */
	NEOGPU_VEC_GPC1_COMPUTE,	/* GPC 1 CTA completion */
	NEOGPU_VEC_GPC2_COMPUTE,	/* GPC 2 CTA completion */
	NEOGPU_VEC_GPC3_COMPUTE,	/* GPC 3 CTA completion */
	NEOGPU_VEC_CE_COMPLETE,		/* Copy Engine DMA done */
	NEOGPU_VEC_ERROR,		/* GPU error/page fault */
	NEOGPU_VEC_RESERVED0,		/* Reserved */
	NEOGPU_VEC_RESERVED1,		/* Reserved */
	NEOGPU_NUM_IRQ_VECTORS
};

/* Interrupt Type Flags (from GPU_REG_INT_STATUS) */
#define NEOGPU_IRQ_GPC0		BIT(0)
#define NEOGPU_IRQ_GPC1		BIT(1)
#define NEOGPU_IRQ_GPC2		BIT(2)
#define NEOGPU_IRQ_GPC3		BIT(3)
#define NEOGPU_IRQ_CE		BIT(4)
#define NEOGPU_IRQ_ERROR	BIT(5)

/**
 * struct neogpu_irq - IRQ management structure
 * @gpu: Back pointer to device
 * @msix_entries: MSI-X table entries
 * @irq_count: Number of allocated IRQs
 * 
 * @compute_wq: Workqueue for compute completion bottom-half
 * @ce_wq: Workqueue for CE completion bottom-half
 * @error_wq: Workqueue for error handling
 * 
 * @irq_enabled: Global IRQ enable flag
 * @handlers_installed: Whether request_irq was called
 */
struct neogpu_irq {
	struct neogpu_device *gpu;
	struct msix_entry msix_entries[NEOGPU_NUM_IRQ_VECTORS];
	int irq_count;
	
	/* Workqueues for bottom-halves */
	struct workqueue_struct *compute_wq;
	struct workqueue_struct *ce_wq;
	struct workqueue_struct *error_wq;
	
	/* Work items (per vector) */
	struct work_struct gpc_work[4];		/* One per GPC */
	struct work_struct ce_work;
	struct work_struct error_work;
	
	/* Status tracking */
	atomic_t irq_pending;			/* Atomic flag for irq handler */
	bool irq_enabled;
	bool handlers_installed;
};

/* Lifecycle */
int neogpu_irq_init(struct neogpu_device *gpu);
void neogpu_irq_fini(struct neogpu_device *gpu);

/* Interrupt Enable/Disable */
void neogpu_irq_enable(struct neogpu_device *gpu);
void neogpu_irq_disable(struct neogpu_device *gpu);

/* Top-half handlers (ISR) */
irqreturn_t neogpu_irq_handler(int irq, void *data);
irqreturn_t neogpu_irq_handler_ce(int irq, void *data);
irqreturn_t neogpu_irq_handler_error(int irq, void *data);

/* Bottom-half work handlers */
void neogpu_compute_work_handler(struct work_struct *work);
void neogpu_ce_work_handler(struct work_struct *work);
void neogpu_error_work_handler(struct work_struct *work);

/* Utility */
const char *neogpu_irq_vector_name(enum neogpu_irq_vector vec);

#endif /* _NEOGPU_IRQ_H_ */