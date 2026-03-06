/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * neogpu_hw.h - NeoGPU Hardware Register Definitions and Operations
 * 
 * Register map corresponds to RTL hub_block.v and giga_thread_scheduler.v
 */

#ifndef _NEOGPU_HW_H_
#define _NEOGPU_HW_H_

#include <linux/types.h>
#include <linux/compiler.h>
#include <linux/wait.h>

/* Forward declaration */
struct neogpu_device;

/* ============================================================================
 * BAR0 Register Offset Definitions (64KB total space)
 * ============================================================================
 * Layout matches RTL hub_block.v configuration register interface
 */

/* Global Control and Status (0x0000 - 0x001F) */
#define GPU_REG_CTRL			0x0000	/* GPU Control */
#define GPU_REG_STATUS			0x0004	/* GPU Status */
#define GPU_REG_INT_MASK		0x0008	/* Interrupt Mask */
#define GPU_REG_INT_STATUS		0x000C	/* Interrupt Status (RO) */

/* Grid Launch Configuration (0x0010 - 0x003F) */
#define GPU_REG_DOORBELL		0x0010	/* Write 1 to launch grid */
#define GPU_REG_GRID_X			0x0020	/* Grid dimension X */
#define GPU_REG_GRID_Y			0x0024	/* Grid dimension Y */
#define GPU_REG_GRID_Z			0x0028	/* Grid dimension Z */
#define GPU_REG_BLOCK_X			0x002C	/* Block dimension X (Warps per block) */
#define GPU_REG_BLOCK_Y			0x0030	/* Block dimension Y */
#define GPU_REG_BLOCK_Z			0x0034	/* Block dimension Z */
#define GPU_REG_KERNEL_ADDR_LO		0x0038	/* Kernel code address [31:0] */
#define GPU_REG_KERNEL_ADDR_HI		0x003C	/* Kernel code address [63:32] */

/* Command Queue Pointers (0x0040 - 0x004F) */
#define GPU_REG_QUEUE_HEAD		0x0040	/* GPU read pointer (RO) */
#define GPU_REG_QUEUE_TAIL		0x0044	/* Driver write pointer */
#define GPU_REG_QUEUE_BASE_LO		0x0048	/* Queue base address [31:0] */
#define GPU_REG_QUEUE_BASE_HI		0x004C	/* Queue base address [63:32] */

/* Interrupt Acknowledge (0x0050 - 0x005F) */
#define GPU_REG_IRQ_ACK			0x0050	/* Write 1 to ack compute IRQ */
#define GPU_REG_IRQ_ACK_CE		0x0054	/* Write 1 to ack CE IRQ */

/* Copy Engine Registers (0x0100 - 0x017F) */
#define GPU_REG_CE_CTRL			0x0100	/* CE Control */
#define GPU_REG_CE_STATUS		0x0104	/* CE Status */
#define GPU_REG_CE_DESC_BASE_LO		0x0108	/* Descriptor ring base [31:0] */
#define GPU_REG_CE_DESC_BASE_HI		0x010C	/* Descriptor ring base [63:32] */
#define GPU_REG_CE_DESC_HEAD		0x0110	/* Descriptor head (RO) */
#define GPU_REG_CE_DESC_TAIL		0x0114	/* Descriptor tail (Driver writes) */

/* GPC Status Registers (0x0200 - 0x027F) */
#define GPU_REG_GPC_STATUS(n)		(0x0200 + ((n) * 0x10))	/* GPC n status */
#define GPU_REG_GPC_CTAS_REMAIN(n)	(0x0204 + ((n) * 0x10))	/* CTAs remaining */
#define GPU_REG_GPC_INST_COUNT_LO(n)	(0x0208 + ((n) * 0x10))	/* Instruction count */
#define GPU_REG_GPC_INST_COUNT_HI(n)	(0x020C + ((n) * 0x10))

/* Performance Counters (0x0300 - 0x037F) */
#define GPU_REG_PERF_L2_HITS		0x0300	/* L2 cache hits */
#define GPU_REG_PERF_L2_MISSES		0x0304	/* L2 cache misses */
#define GPU_REG_PERF_CYCLES		0x0308	/* Total GPU cycles */
#define GPU_REG_PERF_WARP_STALL		0x030C	/* Warp stall count */

/* Debug Registers (0x0380 - 0x03FF) */
#define GPU_REG_DBG_CTRL		0x0380	/* Debug control */
#define GPU_REG_DBG_SM_STATUS		0x0384	/* SM status snapshot */

/* ============================================================================
 * Register Bit Definitions
 * ============================================================================
 */

/* GPU_REG_CTRL bits */
#define GPU_CTRL_ENABLE			BIT(0)	/* GPU global enable */
#define GPU_CTRL_SOFT_RESET		BIT(1)	/* Soft reset trigger */
#define GPU_CTRL_HALT			BIT(2)	/* Halt all execution */
#define GPU_CTRL_FLUSH_CACHE		BIT(3)	/* Flush L2 cache */
#define GPU_CTRL_CE_ENABLE		BIT(4)	/* Copy Engine enable */

/* GPU_REG_STATUS bits */
#define GPU_STATUS_IDLE			BIT(0)	/* GPU is idle (no active CTAs) */
#define GPU_STATUS_IRQ_PENDING		BIT(1)	/* Interrupt pending */
#define GPU_STATUS_CE_IDLE		BIT(2)	/* Copy Engine idle */
#define GPU_STATUS_BUSY			BIT(3)	/* GPU is busy */
#define GPU_STATUS_ERROR		BIT(31)	/* Error condition */

/* GPU_REG_INT_MASK/STATUS bits */
#define GPU_INT_COMPUTE_0		BIT(0)	/* GPC 0 compute complete */
#define GPU_INT_COMPUTE_1		BIT(1)	/* GPC 1 compute complete */
#define GPU_INT_COMPUTE_2		BIT(2)	/* GPC 2 compute complete */
#define GPU_INT_COMPUTE_3		BIT(3)	/* GPC 3 compute complete */
#define GPU_INT_CE_COMPLETE		BIT(4)	/* Copy Engine DMA complete */
#define GPU_INT_ERROR			BIT(5)	/* Error interrupt */
#define GPU_INT_ALL			(0x3F)	/* All interrupts mask */

/* Copy Engine Control bits */
#define CE_CTRL_ENABLE			BIT(0)
#define CE_CTRL_IRQ_ENABLE		BIT(1)
#define CE_CTRL_DIR_H2D			(0 << 2)	/* Host to Device */
#define CE_CTRL_DIR_D2H			(1 << 2)	/* Device to Host */

/* Copy Engine Status bits */
#define CE_STATUS_IDLE			BIT(0)
#define CE_STATUS_BUSY			BIT(1)
#define CE_STATUS_ERROR			BIT(31)

/* ============================================================================
 * Hardware State and Limits
 * ============================================================================
 */
#define NEOGPU_MAX_GPC			4
#define NEOGPU_QUEUE_SIZE		1024	/* Command queue entries */
#define NEOGPU_CE_DESC_SIZE		256	/* CE descriptor ring size */

/* Timeout values (milliseconds) */
#define NEOGPU_RESET_TIMEOUT_MS		1000
#define NEOGPU_IDLE_TIMEOUT_MS		5000
#define NEOGPU_DOORBELL_TIMEOUT_MS	100

/* ============================================================================
 * Function Prototypes
 * ============================================================================
 */

/* Register Access (basic) */
static inline u32 neogpu_read_reg32(struct neogpu_device *gpu, u32 reg);
static inline void neogpu_write_reg32(struct neogpu_device *gpu, u32 reg, u32 val);
static inline u64 neogpu_read_reg64(struct neogpu_device *gpu, u32 reg);
static inline void neogpu_write_reg64(struct neogpu_device *gpu, u32 reg, u64 val);

/* Grid Launch Configuration */
void neogpu_set_grid_dim(struct neogpu_device *gpu, u16 x, u16 y, u16 z);
void neogpu_set_block_dim(struct neogpu_device *gpu, u16 x, u16 y, u16 z);
void neogpu_set_kernel_addr(struct neogpu_device *gpu, u64 addr);
int neogpu_ring_doorbell(struct neogpu_device *gpu);

/* Hardware State Polling */
int neogpu_wait_idle(struct neogpu_device *gpu, u32 timeout_ms);
bool neogpu_is_idle(struct neogpu_device *gpu);
bool neogpu_has_error(struct neogpu_device *gpu);
u32 neogpu_get_status(struct neogpu_device *gpu);

/* Interrupt Management */
void neogpu_ack_irq(struct neogpu_device *gpu, u32 mask);
void neogpu_mask_irq(struct neogpu_device *gpu, u32 mask);
void neogpu_unmask_irq(struct neogpu_device *gpu, u32 mask);

/* Copy Engine Operations */
int neogpu_ce_wait_idle(struct neogpu_device *gpu, u32 timeout_ms);
void neogpu_ce_kick(struct neogpu_device *gpu);

/* Debug and Diagnostics */
void neogpu_dump_regs(struct neogpu_device *gpu);
u64 neogpu_read_perf_counter(struct neogpu_device *gpu, u32 reg);

#endif /* _NEOGPU_HW_H_ */