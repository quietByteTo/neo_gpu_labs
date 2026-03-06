/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * neogpu_sched.h - NeoGPU Command Scheduler and Queue Management
 * 
 * Ring Buffer submission for compute commands (Kernel Launch, Barrier, etc.)
 * Corresponds to RTL hub_block.v Doorbell mechanism and 
 * giga_thread_scheduler.v Grid dispatch.
 */

#ifndef _NEOGPU_SCHED_H_
#define _NEOGPU_SCHED_H_

#include <linux/types.h>
#include <linux/spinlock.h>
#include <linux/dma-fence.h>
#include <linux/wait.h>
#include <linux/kref.h>

/* Command Queue Types */
enum neogpu_queue_type {
	NEOGPU_QUEUE_COMPUTE = 0,	/* Compute/Graphics work */
	NEOGPU_QUEUE_COPY,		/* DMA/Copy Engine work */
	NEOGPU_QUEUE_NUM		/* Max queues */
};

/* Command Types (packed into ring buffer) */
enum neogpu_cmd_type {
	NEOGPU_CMD_NOP = 0,		/* No operation */
	NEOGPU_CMD_KERNEL_LAUNCH,	/* Launch CTA grid */
	NEOGPU_CMD_BARRIER,		/* Execution barrier */
	NEOGPU_CMD_SIGNAL,		/* Signal fence/semaphore */
	NEOGPU_CMD_WAIT,		/* Wait on fence/semaphore */
	NEOGPU_CMD_COPY_BUFFER,		/* Buffer copy (using CE) */
	NEOGPU_CMD_FLUSH_CACHE,		/* Cache flush */
};

/* Command Header (64-bit aligned) */
struct neogpu_cmd_header {
	u32 type;			/* Command type */
	u32 flags;			/* Flags (e.g., IRQ on completion) */
	u64 seqno;			/* Sequence number for ordering */
};

/* Kernel Launch Command Payload */
struct neogpu_cmd_kernel {
	u64 kernel_addr;		/* GPU address of kernel code */
	u32 grid_dim[3];		/* Grid dimensions (CTA count) */
	u32 block_dim[3];		/* Block dimensions (warp count) */
	u64 args_ptr;			/* GPU address of arguments */
	u64 shared_mem_size;		/* Dynamic shared memory */
};

/* Barrier Command */
struct neogpu_cmd_barrier {
	u32 scope;			/* 0=CTA, 1=Grid, 2=System */
	u32 reserved;
};

/* Signal Command (for user fences) */
struct neogpu_cmd_signal {
	u64 fence_addr;			/* GPU address of fence variable */
	u32 value;			/* Value to write */
};

/* Full Command Structure (variable size) */
struct neogpu_command {
	struct neogpu_cmd_header header;
	union {
		struct neogpu_cmd_kernel kernel;
		struct neogpu_cmd_barrier barrier;
		struct neogpu_cmd_signal signal;
	} payload;
};

/**
 * struct neogpu_queue - Command Ring Buffer (per queue type)
 * @type: Queue type (Compute/Copy)
 * @gpu: Device pointer
 * @ring_bo: Buffer object for ring buffer (GPU accessible)
 * @ring_base: CPU virtual address of ring
 * @ring_gpu_addr: GPU physical address of ring
 * @size: Ring size in commands
 * @write_idx: Driver write pointer (producer)
 * @read_idx: GPU read pointer shadow (consumer, updated by IRQ)
 * @lock: Protects ring manipulation
 * @fence_context: DMA fence context for this queue
 * @last_seqno: Last assigned sequence number
 * @pending: Number of pending commands
 * @wait_queue: For blocking submission when ring full
 * @doorbell_reg: Which doorbell register to use (Compute or CE)
 */
struct neogpu_queue {
	enum neogpu_queue_type type;
	struct neogpu_device *gpu;
	
	struct neogpu_bo *ring_bo;
	struct neogpu_command *ring_base;
	u64 ring_gpu_addr;
	
	u32 size;
	u32 write_idx;			/* Cached write pointer */
	volatile u32 read_idx;		/* Shadow of GPU head pointer */
	
	spinlock_t lock;
	u64 fence_context;
	u64 last_seqno;
	u32 pending;
	
	wait_queue_head_t wait_queue;
	u32 doorbell_reg;		/* GPU_REG_DOORBELL or GPU_REG_CE_DESC_TAIL */
};

/**
 * struct neogpu_context - GPU Context (per process)
 * @kref: Reference count
 * @gpu: Device
 * @id: Context ID
 * @vm: Memory management (address space)
 * @queues: Command queues for this context
 * @bo_list: List of buffer objects owned by this context
 * @lock: Protects context state
 * @pid: Owner process ID
 * @name: Context name (for debugging)
 */
struct neogpu_context {
	struct kref kref;
	struct neogpu_device *gpu;
	u32 id;
	
	/* Address space */
	struct neogpu_vm *vm;		/* Virtual memory management */
	
	/* Queues */
	struct neogpu_queue queues[NEOGPU_QUEUE_NUM];
	
	/* Resource tracking */
	struct list_head bo_list;	/* BOs allocated by this context */
	spinlock_t bo_lock;
	
	/* Metadata */
	pid_t pid;
	char name[32];
	u64 submit_count;		/* Commands submitted */
};

/**
 * struct neogpu_vm - Virtual Memory Address Space
 * @start: Start of valid GPU VA range
 * @end: End of valid GPU VA range
 * @pgd: Page directory (if using GPU MMU)
 * @mutex: Protects VM operations
 * 
 * Simple version: Each context gets a 1GB slice of 4GB VRAM
 * Advanced: Full GPU VM with page tables
 */
struct neogpu_vm {
	u64 start;			/* VA start (e.g., 0x40000000 for context 1) */
	u64 end;			/* VA end */
	u32 id;				/* VM/Context ID */
	
	/* Allocation tracking */
	struct rb_root bo_rb;		/* RB tree of allocated regions */
	struct mutex mutex;
};

/* Global Scheduler */
int neogpu_sched_init(struct neogpu_device *gpu);
void neogpu_sched_fini(struct neogpu_device *gpu);

/* Context Management */
struct neogpu_context *neogpu_context_create(struct neogpu_device *gpu, const char *name);
void neogpu_context_destroy(struct kref *kref);
struct neogpu_context *neogpu_context_get(struct neogpu_context *ctx);
void neogpu_context_put(struct neogpu_context *ctx);

/* Queue Operations */
int neogpu_queue_init(struct neogpu_queue *queue, struct neogpu_device *gpu,
		      enum neogpu_queue_type type, u32 size);
void neogpu_queue_fini(struct neogpu_queue *queue);

/* Command Submission */
int neogpu_submit_kernel(struct neogpu_context *ctx,
			 u64 kernel_addr, u32 grid[3], u32 block[3],
			 u64 args_ptr, u64 shared_mem,
			 struct dma_fence **fence);

int neogpu_submit_barrier(struct neogpu_context *ctx, u32 scope);

/* Ring Buffer Management */
void neogpu_ring_doorbell(struct neogpu_queue *queue);
int neogpu_wait_queue_idle(struct neogpu_queue *queue, u32 timeout_ms);

#endif /* _NEOGPU_SCHED_H_ */