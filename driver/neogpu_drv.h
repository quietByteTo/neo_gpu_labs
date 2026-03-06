#ifndef _NEOGPU_DRV_H_
#define _NEOGPU_DRV_H_

#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/pci.h>

/* Forward declarations */
struct neogpu_device;
struct neogpu_mem_mgr;
struct neogpu_ce;  /* Add CE forward decl */

struct neogpu_device {
	struct pci_dev *pdev;
	void __iomem *bar0;
	void __iomem *bar1;
	resource_size_t bar0_start;
	resource_size_t bar1_start;
	
	struct msix_entry msix_entries[8];
	int irq_count;
	
	u32 hw_status;
	bool initialized;
	spinlock_t reg_lock;
	struct device *dev;
	
	struct list_head list;
	
	struct neogpu_mem_mgr *mem_mgr;
	struct neogpu_ce *ce;  /* Add Copy Engine pointer */
};

extern struct list_head neogpu_devices;
extern struct mutex neogpu_devices_lock;

u32 neogpu_read_reg32(struct neogpu_device *gpu, u32 reg);
void neogpu_write_reg32(struct neogpu_device *gpu, u32 reg, u32 val);
u64 neogpu_read_reg64(struct neogpu_device *gpu, u32 reg);
void neogpu_write_reg64(struct neogpu_device *gpu, u32 reg, u64 val);

int neogpu_mem_mgr_init(struct neogpu_device *gpu);
void neogpu_mem_mgr_fini(struct neogpu_device *gpu);

/* CE lifecycle */
int neogpu_ce_init(struct neogpu_device *gpu);
void neogpu_ce_fini(struct neogpu_device *gpu);

#endif /* _NEOGPU_DRV_H_ */