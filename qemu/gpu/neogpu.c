// ============================================================================
// File: hw/gpu/neogpu.c
// Description: QEMU PCIe GPU Device Model for NeoGPU RTL Co-simulation
//              Maps QEMU memory accesses to shared memory TLPs
// ============================================================================
#include "qemu/osdep.h"
#include "qemu/units.h"
#include "hw/pci/pci.h"
#include "hw/pci/msi.h"
#include "hw/qdev-properties.h"
#include "sysemu/sysemu.h"
#include "exec/memory.h"
#include "exec/address-spaces.h"

#include "gpu_cosim.h"  // Shared header

#define TYPE_NEOGPU "neogpu"
#define NEOGPU(obj) OBJECT_CHECK(NeoGPUState, (obj), TYPE_NEOGPU)

typedef struct {
    PCIDevice parent_obj;
    MemoryRegion bar0_mmio;      // 64KB Register space
    MemoryRegion bar1_vram;      // 4GB VRAM window
    
    // Shared Memory
    int shm_fd;
    gpu_shm_region_t *shm;
    
    // State
    uint64_t bar0_addr;          // Programmed BAR0 address
    uint64_t bar1_addr;          // Programmed BAR1 address
    uint32_t grid_x, grid_y, grid_z;
    uint64_t kernel_addr;
    
    // IRQ state
    qemu_irq irq;
    bool irq_pending;
    
} NeoGPUState;

// Forward declarations
static void neogpu_check_response(void *opaque);

// ============================================================================
// MMIO Write Handler (QEMU Guest -> RTL)
// ============================================================================

static uint64_t neogpu_bar0_read(void *opaque, hwaddr addr, unsigned size) {
    NeoGPUState *s = NEOGPU(opaque);
    
    // Check if we should poll RTL for responses first
    neogpu_check_response(s);
    
    switch (addr) {
        case GPU_REG_STATUS:
            return s->shm->rtl_ready ? 0x1 : 0x0;
        case GPU_REG_IRQ_ACK:
            // Acknowledge interrupt
            if (s->irq_pending) {
                s->irq_pending = false;
                qemu_irq_lower(s->irq);
            }
            return 0;
        default:
            return 0;
    }
}

static void neogpu_bar0_write(void *opaque, hwaddr addr, uint64_t val, unsigned size) {
    NeoGPUState *s = NEOGPU(opaque);
    tlp_packet_t *pkt;
    uint32_t tail;
    
    // Queue TLP to RTL
    tail = s->shm->req_tail;
    pkt = &s->shm->req_queue[tail % SHM_QUEUE_DEPTH];
    
    pkt->magic = GPU_MAGIC_REQ;
    pkt->type = TLP_MEM_WRITE;
    pkt->addr = s->bar0_addr + addr;
    
    // Copy data (little-endian)
    memcpy(pkt->data, &val, size);
    pkt->byte_enable = (1 << size) - 1;
    pkt->length = size;
    pkt->tag = tail;
    
    s->shm->req_tail = tail + 1;
    
    // Local state tracking
    switch (addr) {
        case GPU_REG_GRID_X: s->grid_x = val; break;
        case GPU_REG_GRID_Y: s->grid_y = val; break;
        case GPU_REG_GRID_Z: s->grid_z = val; break;
        case GPU_REG_KERNEL_ADDR: s->kernel_addr = val; break;
        case GPU_REG_DOORBELL:
            printf("[QEMU] Doorbell rung! Grid=%dx%dx%d, Kernel=0x%lx\n",
                   s->grid_x, s->grid_y, s->grid_z, s->kernel_addr);
            break;
    }
}

static const MemoryRegionOps neogpu_bar0_ops = {
    .read = neogpu_bar0_read,
    .write = neogpu_bar0_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

// ============================================================================
// VRAM Window (BAR1) - Pass through to RTL
// ============================================================================

static uint64_t neogpu_vram_read(void *opaque, hwaddr addr, unsigned size) {
    NeoGPUState *s = NEOGPU(opaque);
    // For DMA reads, we would wait for RTL response
    // Simplified: return 0 for now
    return 0;
}

static void neogpu_vram_write(void *opaque, hwaddr addr, uint64_t val, unsigned size) {
    NeoGPUState *s = NEOGPU(opaque);
    tlp_packet_t *pkt;
    uint32_t tail;
    
    // Write to VRAM is forwarded to RTL via DMA region
    tail = s->shm->req_tail;
    pkt = &s->shm->req_queue[tail % SHM_QUEUE_DEPTH];
    
    pkt->magic = GPU_MAGIC_REQ;
    pkt->type = TLP_MEM_WRITE;
    pkt->addr = s->bar1_addr + addr;
    memcpy(pkt->data, &val, size);
    pkt->byte_enable = (1 << size) - 1;
    pkt->length = size;
    
    s->shm->req_tail = tail + 1;
}

static const MemoryRegionOps neogpu_vram_ops = {
    .read = neogpu_vram_read,
    .write = neogpu_vram_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 8,
};

// ============================================================================
// Response Polling (RTL -> QEMU)
// ============================================================================

static void neogpu_check_response(void *opaque) {
    NeoGPUState *s = NEOGPU(opaque);
    tlp_packet_t *pkt;
    uint32_t head;
    
    while (s->shm->resp_head != s->shm->resp_tail) {
        head = s->shm->resp_head;
        pkt = &s->shm->resp_queue[head % SHM_QUEUE_DEPTH];
        
        if (pkt->magic != GPU_MAGIC_RESP) {
            s->shm->resp_head = head + 1;
            continue;
        }
        
        if (pkt->type == TLP_IRQ_ASSERT) {
            printf("[QEMU] Interrupt received from RTL!\n");
            s->irq_pending = true;
            qemu_irq_raise(s->irq);
            msi_notify(&s->parent_obj, 0);  // MSI vector 0
        }
        
        s->shm->resp_head = head + 1;
    }
}

// Timer-based polling (every 1ms)
static void neogpu_poll_timer(void *opaque) {
    neogpu_check_response(opaque);
    timer_mod_ms(QEMU_TIMER_PTR(opaque), qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 1);
}

// ============================================================================
// PCI Device Lifecycle
// ============================================================================

static void neogpu_realize(PCIDevice *pdev, Error **errp) {
    NeoGPUState *s = NEOGPU(pdev);
    
    // Setup BAR0 (64KB MMIO)
    memory_region_init_io(&s->bar0_mmio, OBJECT(s), &neogpu_bar0_ops, s,
                          "neogpu-bar0", 64 * KiB);
    pci_register_bar(pdev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->bar0_mmio);
    
    // Setup BAR1 (4GB VRAM window)
    memory_region_init_io(&s->bar1_vram, OBJECT(s), &neogpu_vram_ops, s,
                          "neogpu-vram", 4ULL * GiB);
    pci_register_bar(pdev, 1, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->bar1_vram);
    
    // Setup MSI
    msi_init(pdev, 0, 1, true, false, errp);
    
    // Connect to Shared Memory
    s->shm_fd = shm_open("/gpu_cosim_shm", O_RDWR, 0666);
    if (s->shm_fd < 0) {
        error_setg(errp, "Failed to open shared memory");
        return;
    }
    
    s->shm = mmap(NULL, SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, s->shm_fd, 0);
    if (s->shm == MAP_FAILED) {
        error_setg(errp, "Failed to mmap shared memory");
        return;
    }
    
    s->shm->qemu_ready = 1;
    printf("[QEMU] NeoGPU device initialized, SHM connected\n");
    
    // Start polling timer
    QEMUTimer *timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, neogpu_poll_timer, s);
    timer_mod_ms(timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 10);
}

static void neogpu_unrealize(PCIDevice *pdev) {
    NeoGPUState *s = NEOGPU(pdev);
    if (s->shm) munmap(s->shm, SHM_SIZE);
    if (s->shm_fd >= 0) close(s->shm_fd);
    s->shm->qemu_ready = 0;
}

static void neogpu_class_init(ObjectClass *klass, void *data) {
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);
    DeviceClass *dc = DEVICE_CLASS(klass);
    
    k->realize = neogpu_realize;
    k->unrealize = neogpu_unrealize;
    k->vendor_id = PCI_VENDOR_ID_NEOGPU;  // 0x1234 (example)
    k->device_id = PCI_DEVICE_ID_NEOGPU;  // 0x5678
    k->revision = 0x01;
    k->class_id = PCI_CLASS_DISPLAY_3D;   // 3D controller
    
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
    dc->desc = "NeoGPU RTL Co-simulation Device";
}

static const TypeInfo neogpu_info = {
    .name          = TYPE_NEOGPU,
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(NeoGPUState),
    .class_init    = neogpu_class_init,
    .interfaces = (InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void neogpu_register_types(void) {
    type_register_static(&neogpu_info);
}
type_init(neogpu_register_types)