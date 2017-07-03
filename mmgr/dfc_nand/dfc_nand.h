#ifndef DFC_NAND_H
#define DFC_NAND_H

#include <stdint.h>
#include "../../include/uatomic.h"

#define NAND_PAGE_COUNT         512
#define NAND_SECTOR_COUNT       4
#define NAND_SECTOR_SIZE	0x1000
#define NAND_OOB_SIZE		0x400
#define NAND_EXPOSED_OOB        0x40

/* We assume LUNs * TARGETs for total of LUNs */
#define NAND_VIRTUAL_LUNS       4

#define DFCNAND_RESV_BLK        0
#define DFCNAND_RESV_BLK_COUNT  1

#define DFCNAND_DMA_SLOT_INDEX  64

#define DFCNAND_RDY_BSY_ON      (1 << 3)
#define DFCNAND_RDY_BSY_OFF     0x0

#define DFCNAND_LS2_DMA_DATA   (1 << 0)
#define DFCNAND_LS2_DMA_OOB    (1 << 1)

enum DFCNAND_COMMAND_ID {
    DFCNAND_PAGE_PROG           = 0xA,
    DFCNAND_PAGE_READ           = 0x1E,
    DFCNAND_RESET               = 0x1,
    DFCNAND_READ_STATUS         = 0x14,
    DFCNAND_BLOCK_ERASE         = 0x5,
    DFCNAND_CHANGE_WRITE_COL    = 0xC,
    DFCNAND_SET_FEATURE         = 0x9,
};

enum {
    DFCNAND_READ_IO     = 1,
    DFCNAND_READ_OOB    = 2,
    DFCNAND_WRITE       = 3,
    DFCNAND_READ_STS    = 4,
    DFCNAND_BAD_BLK     = 5,
    DFCNAND_OTHERS      = 6,
    DFCNAND_ERASE       = 7,
    DFCNAND_END_DESC    = 8,
};

struct dfcnand_io {
    uint8_t                  *virt_addr;
    int                      prp_index;
    uint8_t                  cmd_type;
    struct nvm_mmgr_io_cmd   *nvm_mmgr_io;
    void                     *mq_req;
    uint8_t                  rdy_bsy; /* if positive, waits for ready signal */
    uint8_t                  local_dma; /* if > 0, DMA to LS2 memory */
};

#endif /* DFC_NAND_H */