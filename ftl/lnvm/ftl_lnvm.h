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

#include "../../include/ssd.h"

#define FTL_LNVM_IO_RETRY     16

struct lnvm_page {

};

struct lnvm_channel {
    struct nvm_channel     *ch;
};

#endif /* FTL_LNVM_H */