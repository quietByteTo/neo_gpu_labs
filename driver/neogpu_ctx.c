// SPDX-License-Identifier: GPL-2.0-only
/*
 * neogpu_ctx.c - NeoGPU Context Management
 * 
 * Per-process GPU contexts with address space isolation.
 * Simple allocation: Each context gets 1GB slice of 4GB VRAM.
 */

#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/rbtree.h>
#include <linux/pid.h>

#include "neogpu_sched.h"
#include "neogpu_drv.h"

/* Simple address space layout:
 * Context 0: 0x00000000 - 0x3FFFFFFF (1GB)
 * Context 1: 0x40000000 - 0x7FFFFFFF (1GB)
 * Context 2: 0x80000000 - 0xBFFFFFFF (1GB)
 * Context 3: 0xC0000000 - 0xFFFFFFFF (1GB)
 */
#define NEOGPU_CTX_SIZE		(1ULL << 30)  /* 1GB per context */
#define NEOGPU_MAX_CONTEXTS	4

static DEFINE_MUTEX(neogpu_ctx_lock);
static struct neogpu_context *context_table[NEOGPU_MAX_CONTEXTS] = {NULL};

/**
 * neogpu_vm_init - Initialize virtual memory for context
 * @vm: VM structure
 * @ctx_id: Context ID (determines VA range)
 */
static int neogpu_vm_init(struct neogpu_vm *vm, u32 ctx_id)
{
	vm->start = (u64)ctx_id * NEOGPU_CTX_SIZE;
	vm->end = vm->start + NEOGPU_CTX_SIZE;
	vm->id = ctx_id;
	vm->bo_rb = RB_ROOT;
	mutex_init(&vm->mutex);
	
	return 0;
}

/**
 * neogpu_context_create - Create new GPU context
 * @gpu: Device
 * @name: Context name (for debugging)
 * 
 * Return: New context or ERR_PTR
 */
struct neogpu_context *neogpu_context_create(struct neogpu_device *gpu, const char *name)
{
	struct neogpu_context *ctx;
	int ret;
	int i;
	u32 ctx_id;
	
	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return ERR_PTR(-ENOMEM);
	
	/* Allocate context ID */
	mutex_lock(&neogpu_ctx_lock);
	for (i = 0; i < NEOGPU_MAX_CONTEXTS; i++) {
		if (!context_table[i]) {
			ctx_id = i;
			context_table[i] = ctx;
			break;
		}
	}
	mutex_unlock(&neogpu_ctx_lock);
	
	if (i == NEOGPU_MAX_CONTEXTS) {
		kfree(ctx);
		return ERR_PTR(-EBUSY); /* Max contexts reached */
	}
	
	/* Initialize */
	kref_init(&ctx->kref);
	ctx->gpu = gpu;
	ctx->id = ctx_id;
	ctx->pid = current->pid;
	strncpy(ctx->name, name, sizeof(ctx->name) - 1);
	INIT_LIST_HEAD(&ctx->bo_list);
	spin_lock_init(&ctx->bo_lock);
	
	/* Initialize VM */
	ctx->vm = kzalloc(sizeof(*ctx->vm), GFP_KERNEL);
	if (!ctx->vm) {
		ret = -ENOMEM;
		goto err_free_ctx;
	}
	neogpu_vm_init(ctx->vm, ctx_id);
	
	/* Initialize queues */
	ret = neogpu_queue_init(&ctx->queues[NEOGPU_QUEUE_COMPUTE], gpu,
				NEOGPU_QUEUE_COMPUTE, 1024);
	if (ret)
		goto err_free_vm;
	
	ret = neogpu_queue_init(&ctx->queues[NEOGPU_QUEUE_COPY], gpu,
				NEOGPU_QUEUE_COPY, 256);
	if (ret)
		goto err_fini_compute;
	
	dev_info(gpu->dev, "Created context %u (%s) for PID %d, VA [0x%llx - 0x%llx]\n",
		 ctx_id, name, ctx->pid, ctx->vm->start, ctx->vm->end);
	
	return ctx;
	
err_fini_compute:
	neogpu_queue_fini(&ctx->queues[NEOGPU_QUEUE_COMPUTE]);
err_free_vm:
	kfree(ctx->vm);
err_free_ctx:
	mutex_lock(&neogpu_ctx_lock);
	context_table[ctx_id] = NULL;
	mutex_unlock(&neogpu_ctx_lock);
	kfree(ctx);
	return ERR_PTR(ret);
}

/**
 * neogpu_context_destroy - Cleanup context when refcount reaches 0
 */
void neogpu_context_destroy(struct kref *kref)
{
	struct neogpu_context *ctx = container_of(kref, struct neogpu_context, kref);
	struct neogpu_device *gpu = ctx->gpu;
	int i;
	
	dev_info(gpu->dev, "Destroying context %u\n", ctx->id);
	
	/* Wait for queues to drain */
	neogpu_queue_fini(&ctx->queues[NEOGPU_QUEUE_COPY]);
	neogpu_queue_fini(&ctx->queues[NEOGPU_QUEUE_COMPUTE]);
	
	/* Free all BOs (should be empty if userspace behaved) */
	/* TODO: Walk bo_list and free remaining */
	
	/* Free VM */
	kfree(ctx->vm);
	
	/* Remove from table */
	mutex_lock(&neogpu_ctx_lock);
	context_table[ctx->id] = NULL;
	mutex_unlock(&neogpu_ctx_lock);
	
	kfree(ctx);
}

struct neogpu_context *neogpu_context_get(struct neogpu_context *ctx)
{
	if (ctx)
		kref_get(&ctx->kref);
	return ctx;
}

void neogpu_context_put(struct neogpu_context *ctx)
{
	if (ctx)
		kref_put(&ctx->kref, neogpu_context_destroy);
}

/**
 * neogpu_context_alloc_bo - Allocate BO within context's address space
 * @ctx: Context
 * @size: Size in bytes
 * @flags: Allocation flags
 * 
 * Returns BO with GPU address within [vm->start, vm->end]
 */
struct neogpu_bo *neogpu_context_alloc_bo(struct neogpu_context *ctx, 
					  size_t size, u32 flags)
{
	struct neogpu_bo *bo;
	u64 gpu_addr;
	
	/* Allocate base BO */
	bo = neogpu_bo_alloc(ctx->gpu, size, flags);
	if (IS_ERR(bo))
		return bo;
	
	/* Check if address fits in context VM */
	gpu_addr = bo->gpu_addr;
	if (gpu_addr < ctx->vm->start || (gpu_addr + size) > ctx->vm->end) {
		/* Address outside context range - this shouldn't happen 
		 * with simple allocator unless VRAM fragmented */
		dev_warn(ctx->gpu->dev, "BO 0x%llx outside context %u range\n",
			 gpu_addr, ctx->id);
		/* Continue anyway for now, or implement reallocation */
	}
	
	/* Add to context BO list */
	spin_lock(&ctx->bo_lock);
	list_add_tail(&bo->list, &ctx->bo_list);
	spin_unlock(&ctx->bo_lock);
	
	bo->ctx = ctx;  /* Add ctx pointer to bo struct if needed */
	
	return bo;
}

/**
 * neogpu_context_find_bo - Find BO by GPU address in context
 */
struct neogpu_bo *neogpu_context_find_bo(struct neogpu_context *ctx, u64 gpu_addr)
{
	struct neogpu_bo *bo;
	
	spin_lock(&ctx->bo_lock);
	list_for_each_entry(bo, &ctx->bo_list, list) {
		if (bo->gpu_addr == gpu_addr) {
			spin_unlock(&ctx->bo_lock);
			return neogpu_bo_ref(bo);
		}
	}
	spin_unlock(&ctx->bo_lock);
	
	return NULL;
}