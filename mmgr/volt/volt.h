/* OX: Open-Channel NVM Express SSD Controller
 * 
 *  - VOLT: Volatile storage and media manager
 *
 * Copyright 2017 IT University of Copenhagen
 * 
 * Written by Ivan Luiz Picoli <ivpi@itu.dk>
 * 
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef VOLT_SSD_H
#define VOLT_SSD_H

#include <stdint.h>
#include <pthread.h>
#include <ox-mq.h>

#define VOLT_MEM_ERROR      0
#define VOLT_MEM_OK         1
#define VOLT_SECOND         1000000 /* from u-seconds */

#define VOLT_CHIP_COUNT      8
#define VOLT_VIRTUAL_LUNS    4
#define VOLT_BLOCK_COUNT     64
#define VOLT_PAGE_COUNT      64
#define VOLT_SECTOR_COUNT    4
#define VOLT_PLANE_COUNT     2
#define VOLT_PAGE_SIZE       0x4000
#define VOLT_SECTOR_SIZE     0x1000
#define VOLT_SEC_OOB_SIZE    0x10
#define VOLT_OOB_SIZE        (VOLT_SEC_OOB_SIZE * VOLT_SECTOR_COUNT)

#define VOLT_DMA_SLOT_CH     32
#define VOLT_DMA_SLOT_INDEX  (VOLT_DMA_SLOT_CH * VOLT_CHIP_COUNT)
#define VOLT_DMA_READ        0x1
#define VOLT_DMA_WRITE       0x2

/* should be user-defined */
#define VOLT_BLK_LIFE       5000
#define VOLT_RSV_BLK        1

#define VOLT_READ_TIME      50
#define VOLT_WRITE_TIME     200
#define VOLT_ERASE_TIME     1200

#define VOLT_QUEUE_SIZE     2048
#define VOLT_QUEUE_TO       48000

typedef struct VoltStatus {
    uint8_t     ready; /* 0x00-busy, 0x01-ready to use */
    uint8_t     active;
    uint64_t    allocated_memory;
} VoltStatus;

typedef struct VoltPage {
    uint8_t         state; /* 0x00-free, 0x01-alive, 0x02-invalid */
    uint8_t         *data;
} VoltPage;

typedef struct VoltBlock {
    uint16_t        id;
    uint16_t        life; /* available writes before die */
    VoltPage        *next_pg;
    VoltPage        *pages;
} VoltBlock;

typedef struct VoltLun {
    VoltBlock       *blk_offset;
} VoltLun;

typedef struct VoltCh {
    VoltLun         *lun_offset;
} VoltCh;

typedef struct VoltCtrl {
    VoltStatus      status;
    VoltBlock       *blocks;
    VoltLun         *luns;
    VoltCh          *channels;
    struct ox_mq    *mq;
    uint8_t         *edma; /* emergency DMA buffer for timeout requests */
} VoltCtrl;

struct volt_dma {
    uint8_t         *virt_addr;
    uint32_t        prp_index;
    uint8_t         status; /* nand status */
};

#endif /* VOLT_SSD_H */