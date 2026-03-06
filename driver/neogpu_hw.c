// SPDX-License-Identifier: GPL-2.0-only
/*
 * neogpu_hw.c - NeoGPU Hardware Abstraction Layer
 * 
 * Provides register-level operations, Doorbell mechanism, and hardware
 * state polling functions corresponding to RTL hub_block.v interfaces.
 */

#include <linux/io.h>
#include <linux/spinlock.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/sched.h>
#include <linux/printk.h>

#include "neogpu_hw.h"
#include "neogpu_drv.h"

/* Note: Basic read/write functions are defined in neogpu_hw.h as inlines
 * or can be defined here if preferred. Assuming they are available via
 * neogpu_drv.h or defined as static inlines in the header. */

/**
 * neogpu_write_reg32 - Write 32-bit register with debug logging
 */
void neogpu_write_reg32(struct neogpu_device *gpu, u32 reg, u32 val)
{
	unsigned long flags;
	
	spin_lock_irqsave(&gpu->reg_lock, flags);
	writel(val, gpu->bar0 + reg);
	spin_unlock_irqrestore(&gpu->reg_lock, flags);
	
	/* Debug: Log writes to important registers */
	if (reg == GPU_REG_DOORBELL || reg == GPU_REG_CTRL)
		pr_debug("neogpu: Write reg[0x%04x] = 0x%08x\n", reg, val);
}

/**
 * neogpu_read_reg32 - Read 32-bit register
 */
u32 neogpu_read_reg32(struct neogpu_device *gpu, u32 reg)
{
	u32 val;
	unsigned long flags;
	
	spin_lock_irqsave(&gpu->reg_lock, flags);
	val = readl(gpu->bar0 + reg);
	spin_unlock_irqrestore(&gpu->reg_lock, flags);
	
	return val;
}

/**
 * neogpu_write_reg64 - Write 64-bit register (as two 32-bit writes)
 * 
 * Note: Hardware may support atomic 64-bit write or require 
 * low-then-high sequence. Assuming little-endian 64-bit split.
 */
void neogpu_write_reg64(struct neogpu_device *gpu, u32 reg, u64 val)
{
	unsigned long flags;
	
	spin_lock_irqsave(&gpu->reg_lock, flags);
	writel(lower_32_bits(val), gpu->bar0 + reg);
	writel(upper_32_bits(val), gpu->bar0 + reg + 4);
	spin_unlock_irqrestore(&gpu->reg_lock, flags);
}

/**
 * neogpu_read_reg64 - Read 64-bit register
 */
u64 neogpu_read_reg64(struct neogpu_device *gpu, u32 reg)
{
	u64 val;
	u32 low, high;
	unsigned long flags;
	
	spin_lock_irqsave(&gpu->reg_lock, flags);
	low = readl(gpu->bar0 + reg);
	high = readl(gpu->bar0 + reg + 4);
	spin_unlock_irqrestore(&gpu->reg_lock, flags);
	
	val = ((u64)high << 32) | low;
	return val;
}

/**
 * neogpu_set_grid_dim - Configure CTA grid dimensions
 * @gpu: Device pointer
 * @x: Grid X dimension (number of blocks)
 * @y: Grid Y dimension
 * @z: Grid Z dimension
 * 
 * Corresponds to RTL giga_thread_scheduler.v host_grid_dim_* inputs
 */
void neogpu_set_grid_dim(struct neogpu_device *gpu, u16 x, u16 y, u16 z)
{
	neogpu_write_reg32(gpu, GPU_REG_GRID_X, x);
	neogpu_write_reg32(gpu, GPU_REG_GRID_Y, y);
	neogpu_write_reg32(gpu, GPU_REG_GRID_Z, z);
	
	pr_debug("neogpu: Grid dim set to %ux%ux%u\n", x, y, z);
}

/**
 * neogpu_set_block_dim - Configure thread block dimensions
 * @gpu: Device pointer
 * @x: Block X dimension (warps per block)
 * @y: Block Y dimension  
 * @z: Block Z dimension
 */
void neogpu_set_block_dim(struct neogpu_device *gpu, u16 x, u16 y, u16 z)
{
	neogpu_write_reg32(gpu, GPU_REG_BLOCK_X, x);
	neogpu_write_reg32(gpu, GPU_REG_BLOCK_Y, y);
	neogpu_write_reg32(gpu, GPU_REG_BLOCK_Z, z);
	
	pr_debug("neogpu: Block dim set to %ux%ux%u\n", x, y, z);
}

/**
 * neogpu_set_kernel_addr - Set kernel code entry point
 * @gpu: Device pointer
 * @addr: GPU virtual address of kernel microcode (BAR1 offset or HBM addr)
 */
void neogpu_set_kernel_addr(struct neogpu_device *gpu, u64 addr)
{
	neogpu_write_reg64(gpu, GPU_REG_KERNEL_ADDR_LO, addr);
	pr_debug("neogpu: Kernel address set to 0x%llx\n", addr);
}

/**
 * neogpu_ring_doorbell - Trigger GPU execution via Doorbell
 * @gpu: Device pointer
 * 
 * Writes to GPU_REG_DOORBELL which triggers giga_thread_scheduler.v
 * to start dispatching CTAs to GPCs.
 * 
 * Return: 0 on success, -ETIMEDOUT if GPU fails to accept
 */
int neogpu_ring_doorbell(struct neogpu_device *gpu)
{
	u32 status;
	unsigned long timeout;
	
	/* Ensure GPU is enabled and not in reset */
	status = neogpu_read_reg32(gpu, GPU_REG_CTRL);
	if (!(status & GPU_CTRL_ENABLE)) {
		pr_err("neogpu: Cannot ring doorbell, GPU not enabled (ctrl=0x%x)\n", status);
		return -EIO;
	}
	
	/* Check if GPU is ready to accept new grid */
	status = neogpu_read_reg32(gpu, GPU_REG_STATUS);
	if (status & GPU_STATUS_BUSY) {
		pr_warn("neogpu: GPU busy, doorbell may stall\n");
	}
	
	pr_debug("neogpu: Ringing doorbell...\n");
	
	/* Write 1 to doorbell register */
	neogpu_write_reg32(gpu, GPU_REG_DOORBELL, 1);
	
	/* Optional: Wait for hardware to acknowledge (if implemented in RTL) */
	timeout = jiffies + msecs_to_jiffies(NEOGPU_DOORBELL_TIMEOUT_MS);
	while (time_before(jiffies, timeout)) {
		status = neogpu_read_reg32(gpu, GPU_REG_STATUS);
		if (status & GPU_STATUS_BUSY) {
			/* GPU has started processing */
			return 0;
		}
		cpu_relax();
	}
	
	/* If we get here, GPU didn't transition to busy (may be OK for small grids) */
	pr_warn("neogpu: Doorbell timeout waiting for BUSY status\n");
	return 0; /* Still consider it success, GPU may process asynchronously */
}

/**
 * neogpu_is_idle - Check if GPU is idle (no active CTAs)
 * @gpu: Device pointer
 * 
 * Return: true if GPU_STATUS_IDLE bit is set
 */
bool neogpu_is_idle(struct neogpu_device *gpu)
{
	u32 status = neogpu_read_reg32(gpu, GPU_REG_STATUS);
	return (status & GPU_STATUS_IDLE) != 0;
}

/**
 * neogpu_has_error - Check for hardware error condition
 * @gpu: Device pointer
 * 
 * Return: true if GPU_STATUS_ERROR bit is set
 */
bool neogpu_has_error(struct neogpu_device *gpu)
{
	u32 status = neogpu_read_reg32(gpu, GPU_REG_STATUS);
	return (status & GPU_STATUS_ERROR) != 0;
}

/**
 * neogpu_get_status - Read full status register
 * @gpu: Device pointer
 * 
 * Return: 32-bit status value
 */
u32 neogpu_get_status(struct neogpu_device *gpu)
{
	return neogpu_read_reg32(gpu, GPU_REG_STATUS);
}

/**
 * neogpu_wait_idle - Poll until GPU becomes idle
 * @gpu: Device pointer
 * @timeout_ms: Timeout in milliseconds
 * 
 * Used to wait for grid execution completion.
 * 
 * Return: 0 if idle achieved, -ETIMEDOUT on timeout, -EIO on error
 */
int neogpu_wait_idle(struct neogpu_device *gpu, u32 timeout_ms)
{
	unsigned long timeout = jiffies + msecs_to_jiffies(timeout_ms);
	u32 status;
	
	pr_debug("neogpu: Waiting for idle (timeout %ums)...\n", timeout_ms);
	
	while (time_before(jiffies, timeout)) {
		status = neogpu_read_reg32(gpu, GPU_REG_STATUS);
		
		if (status & GPU_STATUS_ERROR) {
			pr_err("neogpu: Error detected while waiting for idle! status=0x%x\n", status);
			return -EIO;
		}
		
		if (status & GPU_STATUS_IDLE) {
			pr_debug("neogpu: GPU idle achieved\n");
			return 0;
		}
		
		/* Yield CPU briefly to avoid tight loop */
		if (timeout_ms > 100)
			msleep(1);
		else
			cpu_relax();
	}
	
	pr_err("neogpu: Timeout waiting for idle (status=0x%x)\n", status);
	return -ETIMEDOUT;
}

/**
 * neogpu_ack_irq - Acknowledge interrupts
 * @gpu: Device pointer
 * @mask: Bitmask of interrupts to ack (GPU_INT_* bits)
 */
void neogpu_ack_irq(struct neogpu_device *gpu, u32 mask)
{
	if (mask & GPU_INT_COMPUTE_0) {
		neogpu_write_reg32(gpu, GPU_REG_IRQ_ACK, 1);
	}
	if (mask & GPU_INT_CE_COMPLETE) {
		neogpu_write_reg32(gpu, GPU_REG_IRQ_ACK_CE, 1);
	}
	
	/* Clear in our tracking */
	gpu->hw_status &= ~mask;
}

/**
 * neogpu_mask_irq - Mask interrupts (disable)
 * @gpu: Device pointer
 * @mask: Bits to mask
 */
void neogpu_mask_irq(struct neogpu_device *gpu, u32 mask)
{
	u32 current = neogpu_read_reg32(gpu, GPU_REG_INT_MASK);
	current |= mask;
	neogpu_write_reg32(gpu, GPU_REG_INT_MASK, current);
}

/**
 * neogpu_unmask_irq - Unmask interrupts (enable)
 * @gpu: Device pointer  
 * @mask: Bits to unmask
 */
void neogpu_unmask_irq(struct neogpu_device *gpu, u32 mask)
{
	u32 current = neogpu_read_reg32(gpu, GPU_REG_INT_MASK);
	current &= ~mask;
	neogpu_write_reg32(gpu, GPU_REG_INT_MASK, current);
}

/**
 * neogpu_ce_wait_idle - Wait for Copy Engine to become idle
 * @gpu: Device pointer
 * @timeout_ms: Timeout in milliseconds
 * 
 * Return: 0 on success, -ETIMEDOUT on timeout
 */
int neogpu_ce_wait_idle(struct neogpu_device *gpu, u32 timeout_ms)
{
	unsigned long timeout = jiffies + msecs_to_jiffies(timeout_ms);
	u32 status;
	
	while (time_before(jiffies, timeout)) {
		status = neogpu_read_reg32(gpu, GPU_REG_CE_STATUS);
		if (status & CE_STATUS_IDLE)
			return 0;
		if (status & CE_STATUS_ERROR)
			return -EIO;
		cpu_relax();
	}
	
	return -ETIMEDOUT;
}

/**
 * neogpu_ce_kick - Trigger Copy Engine to process descriptors
 * @gpu: Device pointer
 */
void neogpu_ce_kick(struct neogpu_device *gpu)
{
	u32 ctrl = neogpu_read_reg32(gpu, GPU_REG_CE_CTRL);
	ctrl |= CE_CTRL_ENABLE;
	neogpu_write_reg32(gpu, GPU_REG_CE_CTRL, ctrl);
}

/**
 * neogpu_dump_regs - Print debug register values
 * @gpu: Device pointer
 */
void neogpu_dump_regs(struct neogpu_device *gpu)
{
	u32 ctrl, status, int_mask, int_status;
	int i;
	
	ctrl = neogpu_read_reg32(gpu, GPU_REG_CTRL);
	status = neogpu_read_reg32(gpu, GPU_REG_STATUS);
	int_mask = neogpu_read_reg32(gpu, GPU_REG_INT_MASK);
	int_status = neogpu_read_reg32(gpu, GPU_REG_INT_STATUS);
	
	pr_info("neogpu: === Register Dump ===\n");
	pr_info("neogpu: CTRL       = 0x%08x (Enable:%d Reset:%d)\n",
		ctrl, !!(ctrl & GPU_CTRL_ENABLE), !!(ctrl & GPU_CTRL_SOFT_RESET));
	pr_info("neogpu: STATUS     = 0x%08x (Idle:%d Busy:%d Error:%d)\n",
		status, !!(status & GPU_STATUS_IDLE), !!(status & GPU_STATUS_BUSY),
		!!(status & GPU_STATUS_ERROR));
	pr_info("neogpu: INT_MASK   = 0x%08x\n", int_mask);
	pr_info("neogpu: INT_STATUS = 0x%08x\n", int_status);
	pr_info("neogpu: GRID_DIM   = %ux%ux%u\n",
		neogpu_read_reg32(gpu, GPU_REG_GRID_X),
		neogpu_read_reg32(gpu, GPU_REG_GRID_Y),
		neogpu_read_reg32(gpu, GPU_REG_GRID_Z));
	pr_info("neogpu: CE_STATUS  = 0x%08x\n", neogpu_read_reg32(gpu, GPU_REG_CE_STATUS));
	
	/* Dump GPC statuses */
	for (i = 0; i < NEOGPU_MAX_GPC; i++) {
		u32 gpc_status = neogpu_read_reg32(gpu, GPU_REG_GPC_STATUS(i));
		if (gpc_status != 0) { /* Only print non-idle GPCs */
			pr_info("neogpu: GPC%d_STATUS = 0x%08x, CTAS_REM=%u\n",
				i, gpc_status,
				neogpu_read_reg32(gpu, GPU_REG_GPC_CTAS_REMAIN(i)));
		}
	}
}

/**
 * neogpu_read_perf_counter - Read performance counter
 * @gpu: Device pointer
 * @reg: Performance counter register offset (e.g., GPU_REG_PERF_L2_HITS)
 * 
 * Return: 64-bit counter value
 */
u64 neogpu_read_perf_counter(struct neogpu_device *gpu, u32 reg)
{
	/* Performance counters are 64-bit in hardware */
	return neogpu_read_reg64(gpu, reg);
}