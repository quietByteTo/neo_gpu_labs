// SPDX-License-Identifier: GPL-2.0-only
/*
 * neogpu_pci.c - NeoGPU PCIe Driver Core
 * 
 * Copyright (c) 2024 NeoGPU Project
 * 
 * PCIe device discovery, BAR mapping, MSI-X initialization,
 * and hardware reset sequence.
 */

#include <linux/module.h>
#include <linux/pci.h>
#include <linux/io.h>
#include <linux/interrupt.h>
#include <linux/msi.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/delay.h>

#include "neogpu_drv.h"

#define DRIVER_NAME	"neogpu"
#define DRIVER_VERSION	"0.1.0"

/* PCI Device IDs */
#define PCI_VENDOR_ID_NEOGPU	0x1234
#define PCI_DEVICE_ID_NEOGPU	0x5678

/* BAR Sizes (from RTL parameters) */
#define NEOGPU_BAR0_SIZE	(64 * 1024)		/* 64KB Registers */
#define NEOGPU_BAR1_SIZE	(4ULL * 1024 * 1024 * 1024) /* 4GB VRAM */

/* MSI-X Vector Definitions (matches RTL hub_block.v) */
enum neogpu_irq_vector {
	NEOGPU_IRQ_COMPUTE_0 = 0,	/* GPC 0 Compute Complete */
	NEOGPU_IRQ_COMPUTE_1,		/* GPC 1 Compute Complete */
	NEOGPU_IRQ_COMPUTE_2,		/* GPC 2 Compute Complete */
	NEOGPU_IRQ_COMPUTE_3,		/* GPC 3 Compute Complete */
	NEOGPU_IRQ_DMA_COMPLETE,	/* Copy Engine DMA Done */
	NEOGPU_IRQ_ERROR,		/* GPU Error/Fault */
	NEOGPU_IRQ_MISC,		/* Miscellaneous */
	NEOGPU_IRQ_RESERVED,		/* Reserved */
	NEOGPU_NUM_IRQ			/* Total: 8 vectors */
};

/* BAR0 Register Offsets (matches RTL hub_block.v) */
#define GPU_REG_CTRL		0x0000
#define GPU_REG_STATUS		0x0004
#define GPU_REG_DOORBELL	0x0010
#define   GPU_CTRL_ENABLE	BIT(0)
#define   GPU_CTRL_SOFT_RESET	BIT(1)
#define   GPU_STATUS_IDLE	BIT(0)
#define   GPU_STATUS_IRQ_PENDING BIT(1)

/**
 * struct neogpu_device - Per-GPU device structure
 * @pdev: Back pointer to PCI device
 * @bar0: Mapped BAR0 (registers)
 * @bar1: Mapped BAR1 (VRAM window)
 * @bar0_start: Physical address of BAR0
 * @bar1_start: Physical address of BAR1
 * @msix_entries: MSI-X table entries
 * @irq_count: Number of allocated IRQs
 * @hw_status: Cached hardware status
 * @initialized: Device successfully initialized flag
 * @lock: Spinlock for register access
 * @dev: Generic device pointer
 */
struct neogpu_device {
	struct pci_dev *pdev;
	void __iomem *bar0;
	void __iomem *bar1;
	resource_size_t bar0_start;
	resource_size_t bar1_start;
	
	struct msix_entry msix_entries[NEOGPU_NUM_IRQ];
	int irq_count;
	
	u32 hw_status;
	bool initialized;
	spinlock_t reg_lock;		/* Protects register access */
	struct device *dev;
};

/* Global device list (for multi-GPU support) */
static LIST_HEAD(neogpu_devices);
static DEFINE_MUTEX(neogpu_devices_lock);

/*
 * Hardware Register Access Helpers
 * Uses spinlock to protect against concurrent access from IRQ handlers
 */
static inline u32 neogpu_read_reg32(struct neogpu_device *gpu, u32 reg)
{
	u32 val;
	unsigned long flags;
	
	spin_lock_irqsave(&gpu->reg_lock, flags);
	val = readl(gpu->bar0 + reg);
	spin_unlock_irqrestore(&gpu->reg_lock, flags);
	return val;
}

static inline void neogpu_write_reg32(struct neogpu_device *gpu, u32 reg, u32 val)
{
	unsigned long flags;
	
	spin_lock_irqsave(&gpu->reg_lock, flags);
	writel(val, gpu->bar0 + reg);
	spin_unlock_irqrestore(&gpu->reg_lock, flags);
}

static inline u64 neogpu_read_reg64(struct neogpu_device *gpu, u32 reg)
{
	u64 val;
	unsigned long flags;
	
	spin_lock_irqsave(&gpu->reg_lock, flags);
	val = readq(gpu->bar0 + reg);
	spin_unlock_irqrestore(&gpu->reg_lock, flags);
	return val;
}

static inline void neogpu_write_reg64(struct neogpu_device *gpu, u32 reg, u64 val)
{
	unsigned long flags;
	
	spin_lock_irqsave(&gpu->reg_lock, flags);
	writeq(val, gpu->bar0 + reg);
	spin_unlock_irqrestore(&gpu->reg_lock, flags);
}

/**
 * neogpu_hardware_reset - Perform soft reset of GPU
 * @gpu: Device to reset
 * 
 * Writes to GPU_REG_CTRL to trigger RTL reset sequence.
 * Waits for device to return to idle state.
 * 
 * Return: 0 on success, -ETIMEDOUT on reset failure
 */
static int neogpu_hardware_reset(struct neogpu_device *gpu)
{
	u32 status;
	int timeout = 1000; /* 1 second timeout */
	
	dev_info(gpu->dev, "Performing hardware reset...\n");
	
	/* Assert soft reset (bit 1) while keeping device disabled */
	neogpu_write_reg32(gpu, GPU_REG_CTRL, GPU_CTRL_SOFT_RESET);
	
	/* Wait for reset to complete (hardware clears bit when done) */
	do {
		status = neogpu_read_reg32(gpu, GPU_REG_CTRL);
		if (!(status & GPU_CTRL_SOFT_RESET))
			break;
		msleep(1);
	} while (--timeout > 0);
	
	if (timeout == 0) {
		dev_err(gpu->dev, "Reset timeout! CTRL=0x%x\n", status);
		return -ETIMEDOUT;
	}
	
	/* Verify device is idle */
	status = neogpu_read_reg32(gpu, GPU_REG_STATUS);
	if (!(status & GPU_STATUS_IDLE)) {
		dev_err(gpu->dev, "Device not idle after reset! STATUS=0x%x\n", status);
		return -EIO;
	}
	
	dev_info(gpu->dev, "Hardware reset complete\n");
	return 0;
}
/* In neogpu_probe(), after neogpu_hardware_reset(): */
	
/* Initialize memory manager */
ret = neogpu_mem_mgr_init(gpu);
if (ret) {
	dev_err(&pdev->dev, "Memory manager init failed: %d\n", ret);
	goto err_msix;
}

/* Store device pointer */
pci_set_drvdata(pdev, gpu);
/**
 * neogpu_init_msix - Initialize MSI-X interrupts
 * @gpu: Device to initialize
 * 
 * Allocates 8 MSI-X vectors as defined in enum neogpu_irq_vector.
 * Vectors are allocated but not yet requested (handlers in later phases).
 * 
 * Return: 0 on success, negative error code on failure
 */
static int neogpu_init_msix(struct neogpu_device *gpu)
{
	struct pci_dev *pdev = gpu->pdev;
	int ret, i;
	
	/* Initialize MSI-X entry structures */
	for (i = 0; i < NEOGPU_NUM_IRQ; i++) {
		gpu->msix_entries[i].entry = i;
		gpu->msix_entries[i].vector = 0;
	}
	
	/* Enable MSI-X */
	ret = pci_enable_msix_exact(pdev, gpu->msix_entries, NEOGPU_NUM_IRQ);
	if (ret) {
		dev_err(gpu->dev, "Failed to enable MSI-X: %d\n", ret);
		return ret;
	}
	
	gpu->irq_count = NEOGPU_NUM_IRQ;
	dev_info(gpu->dev, "MSI-X enabled with %d vectors\n", NEOGPU_NUM_IRQ);
	return 0;
}

/**
 * neogpu_fini_msix - Cleanup MSI-X
 * @gpu: Device to cleanup
 */
static void neogpu_fini_msix(struct neogpu_device *gpu)
{
	if (gpu->irq_count > 0) {
		pci_disable_msix(gpu->pdev);
		gpu->irq_count = 0;
	}
}

/**
 * neogpu_probe - PCI device probe callback
 * @pdev: PCI device
 * @ent: PCI device ID entry
 * 
 * Device initialization sequence:
 * 1. Enable PCI device
 * 2. Request PCI regions
 * 3. Map BAR0 (registers) and BAR1 (VRAM)
 * 4. Initialize MSI-X
 * 5. Perform hardware reset
 * 6. Add to device list
 * 
 * Return: 0 on success, negative error code on failure
 */
static int neogpu_probe(struct pci_dev *pdev, const struct pci_device_id *ent)
{
	struct neogpu_device *gpu;
	int ret;
	
	dev_info(&pdev->dev, "NeoGPU discovered (Vendor: 0x%04X, Device: 0x%04X)\n",
		 pdev->vendor, pdev->device);
	
	/* Allocate device structure */
	gpu = kzalloc(sizeof(*gpu), GFP_KERNEL);
	if (!gpu)
		return -ENOMEM;
	
	gpu->pdev = pdev;
	gpu->dev = &pdev->dev;
	spin_lock_init(&gpu->reg_lock);
	
	/* Enable PCI device */
	ret = pcim_enable_device(pdev);
	if (ret) {
		dev_err(&pdev->dev, "Failed to enable PCI device: %d\n", ret);
		goto err_free;
	}
	
	/* Check BAR sizes */
	if (pci_resource_len(pdev, 0) < NEOGPU_BAR0_SIZE) {
		dev_err(&pdev->dev, "BAR0 too small: %llu < %u\n",
			(u64)pci_resource_len(pdev, 0), NEOGPU_BAR0_SIZE);
		ret = -EINVAL;
		goto err_free;
	}
	
	if (pci_resource_len(pdev, 1) < NEOGPU_BAR1_SIZE) {
		dev_err(&pdev->dev, "BAR1 too small: %llu < %llu\n",
			(u64)pci_resource_len(pdev, 1), (u64)NEOGPU_BAR1_SIZE);
		ret = -EINVAL;
		goto err_free;
	}
	
	/* Request PCI regions */
	ret = pcim_request_regions(pdev, DRIVER_NAME);
	if (ret) {
		dev_err(&pdev->dev, "Failed to request PCI regions: %d\n", ret);
		goto err_free;
	}
	
	/* Map BAR0 (64KB registers) */
	gpu->bar0_start = pci_resource_start(pdev, 0);
	gpu->bar0 = pcim_iomap(pdev, 0, NEOGPU_BAR0_SIZE);
	if (!gpu->bar0) {
		dev_err(&pdev->dev, "Failed to map BAR0\n");
		ret = -ENOMEM;
		goto err_free;
	}
	
	/* Map BAR1 (4GB VRAM) - may fail if not enough vmalloc space,
	 * in that case we'll use dynamic mapping later */
	gpu->bar1_start = pci_resource_start(pdev, 1);
	gpu->bar1 = pcim_iomap(pdev, 1, NEOGPU_BAR1_SIZE);
	if (!gpu->bar1) {
		dev_warn(&pdev->dev, "Failed to map full BAR1 (4GB), will use partial mapping\n");
		/* Try to map first 256MB as fallback */
		gpu->bar1 = pcim_iomap(pdev, 1, 256 * 1024 * 1024);
		if (!gpu->bar1) {
			dev_err(&pdev->dev, "Failed to map BAR1\n");
			ret = -ENOMEM;
			goto err_unmap;
		}
	}
	
	dev_info(&pdev->dev, "BAR0 mapped at %p (phys: 0x%llx)\n", 
		 gpu->bar0, (u64)gpu->bar0_start);
	dev_info(&pdev->dev, "BAR1 mapped at %p (phys: 0x%llx)\n", 
		 gpu->bar1, (u64)gpu->bar1_start);
	
	/* Set DMA mask (64-bit) */
	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
	if (ret) {
		dev_warn(&pdev->dev, "Failed to set 64-bit DMA mask\n");
		ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
		if (ret) {
			dev_err(&pdev->dev, "Failed to set DMA mask\n");
			goto err_unmap;
		}
	}
	
	/* Initialize MSI-X */
	ret = neogpu_init_msix(gpu);
	if (ret) {
		dev_warn(&pdev->dev, "MSI-X init failed, falling back to legacy IRQ\n");
		/* TODO: Add legacy IRQ support if needed */
	}
	
	/* Perform hardware reset */
	ret = neogpu_hardware_reset(gpu);
	if (ret) {
		dev_err(&pdev->dev, "Hardware reset failed: %d\n", ret);
		goto err_msix;
	}
	
	/* Store device pointer in PCI drvdata */
	pci_set_drvdata(pdev, gpu);
	
	/* Add to global device list */
	mutex_lock(&neogpu_devices_lock);
	list_add_tail(&gpu->list, &neogpu_devices);
	mutex_unlock(&neogpu_devices_lock);
	
	gpu->initialized = true;
	dev_info(&pdev->dev, "NeoGPU initialized successfully\n");
	
	return 0;

err_msix:
	neogpu_fini_msix(gpu);
err_unmap:
	/* pcim_iomap automatically unmapped on error */
err_free:
	kfree(gpu);
	return ret;
}

/**
 * neogpu_remove - PCI device remove callback
 * @pdev: PCI device
 * 
 * Cleanup sequence:
 * 1. Remove from device list
 * 2. Disable interrupts
 * 3. Hardware reset (put in safe state)
 * 4. Release resources
 */
static void neogpu_remove(struct pci_dev *pdev)
{
	struct neogpu_device *gpu = pci_get_drvdata(pdev);
	
	if (!gpu)
		return;
	
	dev_info(&pdev->dev, "Removing NeoGPU...\n");
	
	mutex_lock(&neogpu_devices_lock);
	list_del(&gpu->list);
	mutex_unlock(&neogpu_devices_lock);
	
	if (gpu->initialized) {
		neogpu_write_reg32(gpu, GPU_REG_CTRL, 0);
	}
	
	/* Cleanup memory manager */
	neogpu_mem_mgr_fini(gpu);
	
	neogpu_fini_msix(gpu);
	kfree(gpu);
	
	dev_info(&pdev->dev, "NeoGPU removed\n");
}

/* PCI Device ID Table */
static const struct pci_device_id neogpu_pci_ids[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_NEOGPU, PCI_DEVICE_ID_NEOGPU) },
	{ 0, }
};
MODULE_DEVICE_TABLE(pci, neogpu_pci_ids);

/* PCI Driver Structure */
static struct pci_driver neogpu_pci_driver = {
	.name		= DRIVER_NAME,
	.id_table	= neogpu_pci_ids,
	.probe		= neogpu_probe,
	.remove		= neogpu_remove,
	/* .suspend/resume can be added for power management */
};

/**
 * neogpu_pci_init - Module initialization
 */
static int __init neogpu_pci_init(void)
{
	int ret;
	
	pr_info("%s: NeoGPU PCIe Driver v%s\n", DRIVER_NAME, DRIVER_VERSION);
	pr_info("%s: Registering PCI driver...\n", DRIVER_NAME);
	
	ret = pci_register_driver(&neogpu_pci_driver);
	if (ret) {
		pr_err("%s: Failed to register PCI driver: %d\n", DRIVER_NAME, ret);
		return ret;
	}
	
	return 0;
}

/**
 * neogpu_pci_exit - Module cleanup
 */
static void __exit neogpu_pci_exit(void)
{
	pr_info("%s: Unregistering PCI driver...\n", DRIVER_NAME);
	pci_unregister_driver(&neogpu_pci_driver);
}

module_init(neogpu_pci_init);
module_exit(neogpu_pci_exit);

MODULE_AUTHOR("NeoGPU Project");
MODULE_DESCRIPTION("NeoGPU PCIe Driver Core");
MODULE_LICENSE("GPL v2");
MODULE_VERSION(DRIVER_VERSION);