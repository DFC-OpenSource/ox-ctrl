/* OX: Open-Channel NVM Express SSD Controller
 * 
 *  - File Backend: File backed Media Manager
 *
 * Copyright 2024 University of Copenhagen
 * 
 * Written by Ivan Luiz Picoli <ivpi@di.ku.dk>
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

#ifndef FILE_BE_H
#define FILE_BE_H

#include <stdint.h>
#include <pthread.h>
#include <ox-mq.h>

#define FILE_MEM_ERROR      0
#define FILE_MEM_OK         1
#define FILE_SECOND         1000000 /* from u-seconds */

#define FILE_CHIP_COUNT      8
#define FILE_VIRTUAL_LUNS    4
#define FILE_BLOCK_COUNT     64
#define FILE_PAGE_COUNT      64
#define FILE_SECTOR_COUNT    4
#define FILE_PLANE_COUNT     2
#define FILE_PAGE_SIZE       0x4000
#define FILE_SECTOR_SIZE     0x1000
#define FILE_SEC_OOB_SIZE    0x10
#define FILE_OOB_SIZE        (FILE_SEC_OOB_SIZE * FILE_SECTOR_COUNT)

#define FILE_DMA_SLOT_CH     32
#define FILE_DMA_SLOT_INDEX  (FILE_DMA_SLOT_CH * FILE_CHIP_COUNT)
#define FILE_DMA_READ        0x1
#define FILE_DMA_WRITE       0x2

/* should be user-defined */
#define FILE_BLK_LIFE       5000
#define FILE_RSV_BLK        1

#define FILE_READ_TIME      50
#define FILE_WRITE_TIME     200
#define FILE_ERASE_TIME     1200

#define FILE_QUEUE_SIZE     2048
#define FILE_QUEUE_TO       48000

typedef struct FileBEStatus {
    uint8_t     ready; /* 0x00-busy, 0x01-ready to use */
    uint8_t     active;
    uint64_t    allocated_memory;
} FileBEStatus;

typedef struct FileBEPage {
    uint8_t         state; /* 0x00-free, 0x01-alive, 0x02-invalid */
} FileBEPage;

typedef struct FileBEBlock {
    uint32_t        id;
    uint16_t        life; /* available erases before dying */
    uint32_t	    file_id;
    uint64_t	    file_offset;
    FileBEPage     *next_pg;
    FileBEPage     *pages;
} FileBEBlock;

typedef struct FileBELun {
    FileBEBlock    *blk_offset;
} FileBELun;

typedef struct FileBECh {
    FileBELun      *lun_offset;
} FileBECh;

typedef struct FileBECtrl {
    FileBEStatus      status;
    FileBEBlock       *blocks;
    FileBELun         *luns;
    FileBECh          *channels;
    struct ox_mq    *mq;
    uint8_t         *edma; /* emergency DMA buffer for timeout requests */
} FileBECtrl;

struct fbe_dma {
    uint8_t         *virt_addr;
    uint32_t        prp_index;
    uint8_t         status; /* nand status */
};

#endif /* FILE_BE_H */
