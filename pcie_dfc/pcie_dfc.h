#ifndef PCI_DFC_H
#define PCI_DFC_H

#include <stdint.h>
#include "../include/ssd.h"
#include "../include/nvme.h"

#define DEV_MEM   "/dev/mem"
#define DEV_PPMAP "/dev/ppmap"
#define DEV_UIO0  "/dev/uio0"
#define DEV_UIO1  "/dev/uio8"

#define PCIE_MAX_BARS   6
#define PCIE_MAX_EPS	2

/*PCIe3 config space address - According to LS2 DS*/
#define PCI_CONF_ADDR 0x3600000

#define HOST_OUTBOUND_ADDR  0x1400000000        /*Outbound iATU Address*/
#define HOST_OUTBOUND_SIZE  8ULL*1024*1024*1024 /*Outbound iATU size 4GB*/

#define DEV_MEM   "/dev/mem"

#define PCI_OFFS_NVME_REGS     0x0000
#define PCI_OFFS_FIFO_ENTRY    0x0001
#define PCI_OFFS_FIFO_COUNT    0x0002
#define PCI_OFFS_FIFO_RESET    0x0003
#define PCI_OFFS_FIFO_IRQ_CSR  0x0004 /*Unused in Target*/
#define PCI_OFFS_IOSQDBST      0x0005 /*5 to 8: 4 32 bit regs*/
#define PCI_OFFS_GPIO_CSR      0x0009

#define PCI_OFFS_DMA_CSR       0x2000
#define PCI_OFFS_ICR           0x2001 /*Present from PCIe2 only*/
#define PCI_OFFS_DMA_TABLE_SZ  0x2002
#define PCI_OFFS_DMA_TABLE     0x40000

typedef struct DmaRegs {
    uint32_t     *csr[2];
    uint32_t     *icr[2];
    uint32_t     *table_sz[2];
    uint64_t     *table[2];
} DmaRegs;

struct pci_msix_table {
    void         *addr_ptr;
    void         *data_ptr;
    void         *msix_en_ptr;
};

struct pci_msi_table {
    void         *addr_ptr;
    void         *data_ptr;
    void         *msi_en_ptr;
};

struct pci_device {
    uint8_t                      totalBars;
    struct nvm_memory_region     bar[PCIE_MAX_BARS];
};

typedef struct fifo_data {
	uint64_t new_val;
	uint64_t offset;
} fifo_data;

struct pci_ctrl {
    struct pci_device           rc;
    struct pci_device           dev;
    struct nvm_memory_region    io_mem;
    struct nvm_memory_region    msix_mem;
    struct pci_msix_table       msix;
    struct pci_msi_table        msi;
    uint8_t                     irq_mode;
    int                         fd_mem;
    uint32_t                    *fifo_reg;
    uint32_t                    *fifo_count;
    uint32_t                    *fifo_msi;
    uint32_t                    *fifo_reset;
    uint32_t                    *iosqdb_bits;
    uint32_t                    *gpio_csr;
    uint32_t                    *icr[2];
    NvmeRegs                    *nvme_regs;
    DmaRegs                     dma_regs;
};

static inline void reset_iosqdb_bits (struct pci_ctrl *pci)
{
    /*Clear IOSQDB_STATUS REGISTER*/
    int i = 0;
    for(;i<4;i++) memset((void *)(pci->iosqdb_bits + (i * 0x01)), 0,
                                                            sizeof(uint32_t));
}

static inline void reset_fifo (struct pci_ctrl *pci)
{
    /*Clear FIFO*/
    *(pci->fifo_reset) = 0x0;
    *(pci->fifo_reset) = 0x1;
    *(pci->fifo_reset) = 0x0;
}

static inline void reset_nvmeregs (struct pci_ctrl *pci)
{
    /*Clear NVME registers*/
    *(pci->fifo_reset) = 0x0;
    *(pci->fifo_reset) = 0x2;
    *(pci->fifo_reset) = 0x0;
}

#endif /* PCI_DFC_H */