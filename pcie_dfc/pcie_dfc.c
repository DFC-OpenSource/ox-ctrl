/* OX: Open-Channel NVM Express SSD Controller
 *
 *  - DFC PCIe Handler
 *
 * Copyright (C) 2016, IT University of Copenhagen. All rights reserved.
 * Written by Ivan Luiz Picoli <ivpi@itu.dk>
 * This file has been modified from the VVDN issd-nvme controller.
 *
 * Funding support provided by CAPES Foundation, Ministry of Education
 * of Brazil, Brasilia - DF 70040-020, Brazil.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  - Redistributions of source code must retain the above copyright notice,
 *  this list of conditions and the following disclaimer.
 *  - Redistributions in binary form must reproduce the above copyright notice,
 *  this list of conditions and the following disclaimer in the documentation
 *  and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef __USE_GNU
#define __USE_GNU
#endif
#define _GNU_SOURCE

#include <pthread.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <syslog.h>
#include <string.h>
#include <mqueue.h>
#include <sched.h>
#include "../include/ssd.h"
#include "pcie_dfc.h"

extern struct core_struct core;

static uint8_t nvmeKilled = 0;

static inline void pcie_nvme_proc (uint32_t *fc_ptr, struct pci_ctrl *pci,
                                                                NvmeCtrl *n) {
    int         j;
    uint32_t    arr[4];
    fifo_data   entry;

    uint16_t fifo_count = *fc_ptr;
    while (fifo_count) {
        for (j=0; j<4; j++){
            arr[j] = *pci->fifo_reg;
        }
        entry.new_val = *(uint64_t *)arr;
        entry.offset =  (arr[2] & 0xffffffff);
        if((entry.offset == 0) || (entry.offset > 0x200)) {
            log_info("[pci: Wrong offs: 0x%lx clearing FIFO]\n",
                                                (long unsigned)entry.offset);
            reset_fifo(pci);
            reset_iosqdb_bits(pci);
            fifo_count = *fc_ptr;
            continue;
        }
        fifo_count -= 4;
        if ((entry.offset << 3) < 0x1000) {
            if(arr[3] == 0xc0000000) {
                nvme_process_reg (n, entry.offset << 3, entry.new_val);
            } else if (arr[3] == 0x80000000) {
                nvme_process_reg (n, (entry.offset << 3) + 0x4 ,
                                                        (entry.new_val >> 32));
            } else {
                nvme_process_reg (n, entry.offset << 3,
                                                 (entry.new_val & 0xffffffff));
            }
        } else {
            nvme_process_db (n, entry.offset << 3, entry.new_val);
        }
    }
}

static inline int pcie_uio_write_int (ssize_t *nb, int fd,
                                                       uint32_t *intr_enable) {
    *nb = write(fd, intr_enable, sizeof(*intr_enable));
    if (*nb < sizeof(*intr_enable)) {
        log_err("[pci: uio write error]\n");
        return 1;
    }
    return 0;
}

static inline int pcie_uio_read_int (ssize_t *nb, int fd, uint32_t *int_no) {
    *nb = read(fd, int_no, sizeof(*int_no));
    if(*nb != sizeof(*int_no)){
        log_err("[pci: uio read error]\n");
        return 1;
    }
    return 0;
}

void *dfcpcie_req_processor (void *arg)
{
    struct pci_ctrl *pci = (struct pci_ctrl *) arg;
    struct timeval  def_time = {0, 100}, timeout = {0, 100};
    NvmeCtrl        *n = core.nvm_nvme_ctrl;
    uint32_t        *fifo_count_ptr = pci->fifo_count;
    ssize_t         nb;
    uint32_t        int_no, intr_enable = 1;
    int             uiofd, uiofd1, rc;

    uiofd = open("/dev/uio2", O_RDWR);
    if (uiofd < 0)
        goto CLEAN_EXIT;

    uiofd1 = open("/dev/uio3", O_RDWR);
    if(uiofd1 < 0)
        goto CLEAN_EXIT;

    int max = (uiofd > uiofd1) ? uiofd : uiofd1;
    fd_set readfds;

    do {
        if(pcie_uio_write_int (&nb, uiofd, &intr_enable))
            continue;
    } while (nb < 0);

    do {
	if(pcie_uio_write_int (&nb, uiofd1, &intr_enable))
            continue;
    } while (nb < 0);

    FD_ZERO(&readfds);

    log_info( "  [pci: NVMe thread alive.]\n");
    while (!nvmeKilled) {
        if (!n || !n->running)
            continue;
        FD_SET(uiofd, &readfds);
        FD_SET(uiofd1, &readfds);
        rc = select(max+1, &readfds, NULL, NULL, &timeout);
        if(rc == -1) {
            timeout = def_time;
            continue;
        } else if (rc == 0) {
            pcie_nvme_proc (fifo_count_ptr, pci, n);

            nvme_q_scheduler(n,((struct pci_ctrl *)core.nvm_pcie->ctrl)
                                                                ->fifo_count);
            timeout = def_time;
        } else {
            if(FD_ISSET(uiofd, &readfds)) {
                if(pcie_uio_read_int (&nb, uiofd, &int_no))
                    continue;

                pcie_nvme_proc (fifo_count_ptr, pci, n);

                if(pcie_uio_write_int (&nb, uiofd, &intr_enable))
                    continue;
            }
            if(FD_ISSET(uiofd1, &readfds)) {
                if(pcie_uio_read_int (&nb, uiofd1, &int_no))
                    continue;

                nvme_q_scheduler(n, ((struct pci_ctrl *) core.nvm_pcie->ctrl)
                                                                 ->fifo_count);
                if(pcie_uio_write_int (&nb, uiofd1, &intr_enable))
                    continue;
            }
        }
    }
CLEAN_EXIT:
    close(uiofd);
    close(uiofd1);
    log_info("  [pci: NVMe thread killed.]\n");
    return;
}

static void pcie_munmap_host_mem (struct pci_ctrl *pci)
{
	if (pci->io_mem.addr) {
		munmap ((void *)pci->io_mem.addr, pci->io_mem.size);
	}
	if (pci->msix_mem.addr) {
		munmap ((void *)pci->msix_mem.addr, pci->msix_mem.size);
	}
	close (pci->fd_mem);
	memset (pci, 0, sizeof (struct pci_ctrl));
}

static int pcie_mmap_host_mem (struct pci_ctrl *pci)
{
    int fd_host_mem = 0;

    void *mem_addr;

    uint64_t msix_mem_addr = PCI_CONF_ADDR;

    memset(pci, 0, sizeof (struct pci_ctrl));

    fd_host_mem = open(DEV_MEM, O_RDWR);
    if (fd_host_mem < 0)
        goto CLEAN_EXIT;

    pci->fd_mem = fd_host_mem;

    log_info("  [pci: outbound addr: 0x%lx  host size: 0x%llx]\n",
                            (uint64_t)HOST_OUTBOUND_ADDR, HOST_OUTBOUND_SIZE);

    /*Host io memory*/
    mem_addr = mmap(0, HOST_OUTBOUND_SIZE, PROT_READ | PROT_WRITE,
                            MAP_SHARED, fd_host_mem, HOST_OUTBOUND_ADDR);

    if (mem_addr == NULL)
        goto CLEAN_EXIT;

    log_info("  [pci: host io_mem_addr : %p]\n", mem_addr);

    pci->io_mem.addr = (uint64_t)mem_addr;
    pci->io_mem.paddr = HOST_OUTBOUND_ADDR;
    pci->io_mem.size = HOST_OUTBOUND_SIZE;
    pci->io_mem.is_valid = 1;

    /*Host msi(x) table info memory*/
    mem_addr = mmap(0, getpagesize (), PROT_READ | PROT_WRITE,
                            MAP_SHARED, fd_host_mem, msix_mem_addr);

    if(mem_addr == NULL)
        goto CLEAN_EXIT;

    log_info("  [pci: host msix_mem_addr : %p]\n", mem_addr);

    pci->msix_mem.addr = (uint64_t)mem_addr;
    pci->msix_mem.size = getpagesize ();
    pci->msix_mem.is_valid = 1;

    pci->msix.addr_ptr = mem_addr + 0x948;
    pci->msix.msix_en_ptr = mem_addr + 0xb2;

    pci->msi.msi_en_ptr = mem_addr + 0x52;
    pci->msi.addr_ptr = mem_addr + 0x54;
    pci->msi.data_ptr = mem_addr + 0x5c;

    return 0;

CLEAN_EXIT:
    pcie_munmap_host_mem (pci);

    return -1;
}

static void pcie_munmap_bars (struct pci_ctrl *pci)
{
    struct pci_device *ep = &pci->dev;
    int i = 0;
    struct nvm_memory_region *bar;

    bar = ep->bar;
    for (i = 0; i < PCIE_MAX_BARS; i++) {
        if (bar[i].is_valid) {
            munmap ((void *)bar[i].addr, bar[i].size);
        }
    }
    memset (pci, 0, sizeof (struct pci_ctrl));
}

static int pcie_mmap_bars (struct pci_ctrl *pci)
{
    int i;
    int nPages[PCIE_MAX_BARS] = {1, 1, 32, 1, 32, 1};
    int fd_mem;
    struct nvm_memory_region *bar;
    struct pci_device *ep = &pci->dev;

    fd_mem = open (DEV_UIO0, O_RDWR);
    if(fd_mem < 0)
        return -1;

    bar = ep->bar;
    for (i=0; i < PCIE_MAX_BARS; i++) {
        bar[i].size  = nPages[i] * getpagesize();
        bar[i].addr = (uint64_t)mmap (0, bar[i].size, PROT_READ |
                        PROT_WRITE, MAP_SHARED, fd_mem, i * getpagesize());
        if (bar[i].addr == (uint64_t)MAP_FAILED) {
            memset (&bar[i].addr, 0, sizeof (uint64_t));
            bar[i].is_valid = 0;
        } else
            bar[i].is_valid = 1;
    }

    /*Now verify the required BARs are mapped properly*/
    if (!pci->dev.bar[2].is_valid ||
        !pci->dev.bar[4].is_valid)
        goto BADBAR;

    return 0;

BADBAR:
    log_err("[ERROR: pci: Bad BAR\n");
CLEAN_EXIT:
    pcie_munmap_bars (pci);
    return -1;
}

static void pcie_setup_regs (struct pci_ctrl *pci)
{
    int i;
    struct pci_device *ep = &pci->dev;
    DmaRegs *dma_regs = &pci->dma_regs;
    pci->nvme_regs = (typeof (pci->nvme_regs))(ep->bar[2].addr + 0x2000 + \
			PCI_OFFS_NVME_REGS);

    pci->fifo_reg = (uint32_t *)ep->bar[4].addr + PCI_OFFS_FIFO_ENTRY;
    pci->fifo_count = (uint32_t *)ep->bar[4].addr + PCI_OFFS_FIFO_COUNT;
    pci->fifo_reset = (uint32_t *)ep->bar[4].addr + PCI_OFFS_FIFO_RESET;
    pci->fifo_msi = (uint32_t *)ep->bar[4].addr + PCI_OFFS_FIFO_IRQ_CSR;
    pci->iosqdb_bits = (uint32_t *)ep->bar[4].addr + PCI_OFFS_IOSQDBST;
    pci->gpio_csr = (uint32_t *)ep->bar[4].addr + PCI_OFFS_GPIO_CSR;

    for (i = 0; i < 1; i++) {
    	dma_regs->icr[i] = (uint32_t *)ep->bar[4].addr + PCI_OFFS_ICR;
	pci->icr[i] = dma_regs->icr[i];
	dma_regs->table_sz[i] = (uint32_t *)ep->bar[4].addr +
                                                        PCI_OFFS_DMA_TABLE_SZ;
	dma_regs->csr[i] = (uint32_t *)ep->bar[4].addr + PCI_OFFS_DMA_CSR;
	dma_regs->table[i] = (uint64_t *)ep->bar[2].addr + PCI_OFFS_DMA_TABLE;
    }

    log_info("  [pci: bar4:0x%lx icr:%p table_sz:%p csr:%p table:%p \n",
	ep->bar[4].addr, dma_regs->icr[0], dma_regs->table_sz[0],
        dma_regs->csr[0], dma_regs->table[0]);
}

static int pcie_init_pci (struct pci_ctrl *ctrl)
{
    if(pcie_mmap_host_mem(ctrl)) {
        log_err("[ERROR: pci: mmap host mem\n");
        return -1;
    }

    if(pcie_mmap_bars(ctrl)) {
        log_err("[ERROR: pci: mmap pci BARs\n");
        return -1;
    }

    pcie_setup_regs(ctrl);

    return 0;
}

struct nvm_pcie pcie_dfc = {
    .name           = "PCI_LS2085",
};

static void dfcpcie_isr_notify (void *opaque)
{
    NvmeCQ *cq = opaque;
    struct pci_ctrl *pci = (struct pci_ctrl *) pcie_dfc.ctrl;

    if (cq->irq_enabled) {
	uint32_t vector_addr;
	uint16_t *data_addr;
	if ((*(uint16_t *)(pci->msix.msix_en_ptr)) & 0x8000) {
            *(uint32_t *)(pci->msix.addr_ptr) = cq->vector;
	} else if ((*(uint16_t *)(pci->msi.msi_en_ptr)) & 0x1) {
            /*Multiple MSIs to be tested yet -TODO*/
            data_addr = pci->msi.data_ptr;
            vector_addr = *(uint32_t *)(pci->msi.addr_ptr);
            *(vector_addr + (char *)pci->io_mem.addr) = *data_addr;
	} else {
            /*TODO: Should raise INTX? */
            return;
	}
    }
}

static uint32_t *dfcpcie_get_iodbst_reg (void)
{
    return ((struct pci_ctrl *)pcie_dfc.ctrl)->iosqdb_bits;
}

static void dfcpcie_exit() {
    struct pci_ctrl *pcie = (struct pci_ctrl *) pcie_dfc.ctrl;
    reset_iosqdb_bits(pcie);
    reset_nvmeregs(pcie);
    reset_fifo(pcie);
    nvmeKilled = 0;
    pcie_munmap_bars(pcie);
    pcie_munmap_host_mem (pcie);
    if (pcie->dev.bar[2].addr) {
	pcie_dfc.nvme_regs->vBar.cc = 0;
    }
    free(pcie);
}

struct nvm_pcie_ops pcidfc_ops = {
    .nvme_consumer      = dfcpcie_req_processor,
    .exit               = dfcpcie_exit,
    .isr_notify         = dfcpcie_isr_notify
};

int dfcpcie_init()
{
    pcie_dfc.ctrl = (void *)calloc(sizeof(struct pci_ctrl), 1);
    if (!pcie_dfc.ctrl)
        return EMEM;

    if(pcie_init_pci(pcie_dfc.ctrl))
        return EPCIE_REGISTER;

    pcie_dfc.nvme_regs = ((struct pci_ctrl *) pcie_dfc.ctrl)->nvme_regs;
    pcie_dfc.host_io_mem = &((struct pci_ctrl *) pcie_dfc.ctrl)->io_mem;
    pcie_dfc.ops = &pcidfc_ops;
    pcie_dfc.io_dbstride_ptr = dfcpcie_get_iodbst_reg();

    return nvm_register_pcie_handler(&pcie_dfc);
}
