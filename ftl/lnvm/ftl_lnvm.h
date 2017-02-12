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

#define FTL_LNVM_IO_RETRY     0
#define FTL_LNVM_RSV_BLK      1

enum {
    FTL_PGMAP_OFF   = 0,
    FTL_PGMAP_ON    = 1
};

struct lnvm_page {

};

struct lnvm_channel {
    struct nvm_channel       *ch;
    uint8_t                  *bbtbl;
    LIST_ENTRY(lnvm_channel) entry;
};

#endif /* FTL_LNVM_H */
