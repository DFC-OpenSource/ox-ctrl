/* OX: Open-Channel NVM Express SSD Controller
 *
 *  - Open-Channel SSD Media Manager (header)
 *
 * Copyright (c) 2018, Microsoft Research
 * Written by Ivan Luiz Picoli <ivpi@itu.dk>
 * Written by Niclas Hedam <nhed@itu.dk>
 */

#ifndef MMGR_OCSSD_H
#define MMGR_OCSSD_H

#include <liblightnvm.h>

#define OCSSD_FILE  "/dev/nvme0n1\0"

#define OCSSD_QUEUE_COUNT 128
#define OCSSD_QUEUE_SIZE  2048
#define OCSSD_QUEUE_TO    400000

struct ocssd_ctrl {
    char                     dev_name[13];
    struct nvm_dev          *dev;
    const struct nvm_geo    *geo;
    struct nvm_mmgr_geometry mmgr_geo;
    struct nvm_mmgr_ops      mmgr_ops;
    struct nvm_mmgr          mmgr;
    struct ox_mq            *mq;
};

#endif /* MMGR_OCSSD_H */
