// ============================================================================
// File: gpu_cosim.h
// Description: Shared Memory Protocol for QEMU-Verilator Co-simulation
// ============================================================================
#ifndef GPU_COSIM_H
#define GPU_COSIM_H

#include <stdint.h>

// Magic numbers for synchronization
#define GPU_MAGIC_REQ   0x47505552  // "GPUR"
#define GPU_MAGIC_RESP  0x47505553  // "GPUS"
#define GPU_MAGIC_IRQ   0x47505549  // "GPUI"

// TLP Types
typedef enum {
    TLP_MEM_READ  = 0x00,
    TLP_MEM_WRITE = 0x01,
    TLP_CONFIG_RD = 0x02,
    TLP_CONFIG_WR = 0x03,
    TLP_IRQ_ASSERT= 0x04,  // RTL -> QEMU interrupt
    TLP_IRQ_DEASSERT=0x05,
    TLP_DMA_REQ   = 0x10,  // Copy Engine DMA request
    TLP_DMA_DONE  = 0x11,
    TLP_SYNC      = 0xFF   // Time synchronization
} tlp_type_t;

// Shared Memory Layout (1MB shm region)
#define SHM_SIZE        (1024 * 1024)
#define SHM_QUEUE_DEPTH 256

typedef struct {
    uint32_t magic;         // GPU_MAGIC_REQ or GPU_MAGIC_RESP
    uint32_t type;          // tlp_type_t
    uint64_t addr;          // Address (BAR offset or physical)
    uint32_t data[16];      // Up to 64 bytes (512-bit) data
    uint32_t byte_enable;   // Byte enable mask
    uint32_t length;        // Length in bytes
    uint32_t tag;           // Transaction ID for ordering
} tlp_packet_t;

// Shared Memory Region Layout
typedef struct {
    // Control (0x0000 - 0x0FFF)
    volatile uint32_t qemu_ready;       // QEMU sets when initialized
    volatile uint32_t rtl_ready;        // Verilator sets when initialized
    volatile uint64_t qemu_time;        // QEMU instruction count (sync)
    volatile uint64_t rtl_cycles;       // RTL cycle count (sync)
    volatile uint32_t irq_pending;      // RTL -> QEMU interrupt status
    
    // Request Queue: QEMU -> RTL (0x1000 - 0x4FFF)
    volatile uint32_t req_head;         // QEMU writes
    volatile uint32_t req_tail;         // RTL reads
    tlp_packet_t      req_queue[SHM_QUEUE_DEPTH];
    
    // Response Queue: RTL -> QEMU (0x5000 - 0x8FFF)
    volatile uint32_t resp_head;        // RTL writes
    volatile uint32_t resp_tail;        // QEMU reads
    tlp_packet_t      resp_queue[SHM_QUEUE_DEPTH];
    
    // DMA Buffer (0x9000 - end): Shared DMA data region
    uint8_t           dma_buffer[SHM_SIZE - 0x9000];
    
} gpu_shm_region_t;

// PCIe Configuration Space offsets (BAR0)
#define GPU_REG_CTRL        0x0000
#define GPU_REG_STATUS      0x0004
#define GPU_REG_DOORBELL    0x0010  // Write here to launch kernel
#define GPU_REG_GRID_X      0x0020
#define GPU_REG_GRID_Y      0x0024
#define GPU_REG_GRID_Z      0x0028
#define GPU_REG_KERNEL_ADDR 0x0030
#define GPU_REG_IRQ_ACK     0x0040

// BAR1: VRAM Window (4GB)
#define GPU_VRAM_BASE       0x00000000

#endif // GPU_COSIM_H