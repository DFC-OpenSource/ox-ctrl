/* OX: OpenChannel NVM Express SSD Controller
 *
 * Copyright (C) 2016, IT University of Copenhagen. All rights reserved.
 * Written by Ivan Luiz Picoli <ivpi@itu.dk>
 *
 * Funding support provided by CAPES Foundation, Ministry of Education
 * of Brazil, Brasilia - DF 70040-020, Brazil.
 *
 * This code is licensed under the GNU GPL v2 or later.
 */

#ifndef FTL_LNVM_H
#define FTL_LNVM_H

#include <sys/queue.h>
#include "../../include/ssd.h"

#define FTL_LNVM_IO_RETRY       0
#define FTL_LNVM_RSV_BLK        1
#define FTL_LNVM_RSV_BLK_COUNT  1
#define FTL_LNVM_MAGIC          0x3c

enum {
    FTL_PGMAP_OFF   = 0,
    FTL_PGMAP_ON    = 1
};

enum lnvm_bbt_state {
    NVM_BBT_FREE = 0x0, // Block is free AKA good
    NVM_BBT_BAD  = 0x1, // Block is bad
    NVM_BBT_GBAD = 0x2, // Block has grown bad
    NVM_BBT_DMRK = 0x4, // Block has been marked by device side
    NVM_BBT_HMRK = 0x8  // Block has been marked by host side
};

#define LNVM_BBT_EMERGENCY   0x0 // Creates the bbt without erasing the channel
#define LNVM_BBT_ERASE       0x1 // Checks for bad blocks only erasing the block
#define LNVM_BBT_FULL        0x2 // Checks for bad blocks erasing the block,
                                 //   writing and reading all pages,
                                 //   and comparing the buffers


struct lnvm_page {

};

struct lnvm_bbtbl {
    uint8_t magic;
    uint32_t bb_sz;
    uint16_t bb_count;
    /* This struct is stored on NVM up to this point, *tbl is not stored */
    uint8_t  *tbl;
};

struct lnvm_channel {
    struct nvm_channel       *ch;
    struct lnvm_bbtbl        *bbtbl;
    LIST_ENTRY(lnvm_channel) entry;
};

int lnvm_get_bbt_nvm (struct lnvm_channel *, struct lnvm_bbtbl *);
int lnvm_bbt_create (struct lnvm_channel *, struct lnvm_bbtbl *, uint8_t);
int lnvm_flush_bbt (struct lnvm_channel *, struct lnvm_bbtbl *);

#endif /* FTL_LNVM_H */