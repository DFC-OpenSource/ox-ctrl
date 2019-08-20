/* OX: Open-Channel NVM Express SSD Controller
 * 
 *  - LightNVM Flash Translation Layer (header)
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

#ifndef FTL_LNVM_H
#define FTL_LNVM_H

#include <sys/queue.h>
#include <libox.h>

#define FTL_LNVM_IO_RETRY       0
#define FTL_LNVM_RSV_BLK        1
#define FTL_LNVM_RSV_BLK_COUNT  1
#define FTL_LNVM_MAGIC          0x3c

enum {
    FTL_PGMAP_OFF   = 0,
    FTL_PGMAP_ON    = 1
};

#define LNVM_BBT_EMERGENCY   0x0 // Creates the bbt without erasing the channel
#define LNVM_BBT_ERASE       0x1 // Checks for bad blocks only erasing the block
#define LNVM_BBT_FULL        0x2 // Checks for bad blocks erasing the block,
                                 //   writing and reading all pages,
                                 //   and comparing the buffers

struct lnvm_page {

};

struct lnvm_bbtbl {
    uint32_t rsvd;
    uint32_t magic;
    uint32_t bb_sz;
    uint32_t bb_count;
    /* This struct is stored on NVM up to this point, *tbl is not stored */
    uint8_t  *tbl;
} __attribute__((packed));

struct lnvm_channel {
    struct nvm_channel       *ch;
    struct lnvm_bbtbl        *bbtbl;
    LIST_ENTRY(lnvm_channel) entry;
};

int lnvm_get_bbt_nvm (struct lnvm_channel *);
int lnvm_bbt_create (struct lnvm_channel *, struct lnvm_bbtbl *, uint8_t);
int lnvm_flush_bbt (struct lnvm_channel *);

#endif /* FTL_LNVM_H */