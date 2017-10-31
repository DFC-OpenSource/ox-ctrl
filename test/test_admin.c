/* OX: Open-Channel NVM Express SSD Controller
 *
 *  - OX Administration Module
 *
 * Copyright (C) 2016, IT University of Copenhagen. All rights reserved.
 * Written by Ivan Luiz Picoli <ivpi@itu.dk>
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../include/tests.h"
#include "../include/ssd.h"
#include "../ftl/lnvm/ftl_lnvm.h"

extern struct core_struct core;

static void oxadmin_show_all ()
{
    printf(" \nAvailable OX Admin Tasks: \n");
    printf("\n  - 'erase-blk': erase specific blocks.");
    printf("\n     eg. ox-ctrl admin -t erase-blk\n");
    printf("\n  - 'create-bbt': create or update the bad block table.");
    printf("\n     eg. ox-ctrl admin -t create-bbt\n\n");
}

static int oxadmin_confirm_erase (int n, struct nvm_ppa_addr *ppa)
{
    char choice;
    printf("\n You asked for erasing %d blocks.\n", n);
    printf(" ppa: ch: %d, lun %d, blk: %d\n",ppa->g.ch, ppa->g.lun, ppa->g.blk);
    printf(" Are you sure? (Y or N): ");
    scanf (" %c", &choice);

    if (choice == 0x59 || choice == 0x79)
        return 0;

    return -1;
}

static int oxadmin_erase_blk (struct nvm_channel *ch, struct nvm_ppa_addr *ppa)
{
    int ret, pl;
    struct nvm_mmgr_io_cmd *cmd = malloc(sizeof(struct nvm_mmgr_io_cmd));
    if (!cmd)
        return EMEM;

    for (pl = 0; pl < ch->geometry->n_of_planes; pl++) {
        memset (cmd, 0, sizeof (struct nvm_mmgr_io_cmd));
        cmd->ppa.ppa = ppa->ppa;
        cmd->ppa.g.pl = pl;
        cmd->ppa.g.ch = ch->ch_mmgr_id;
        cmd->ppa.g.pg = 0;

        ret = nvm_submit_sync_io (ch, cmd, NULL, MMGR_ERASE_BLK);
        if (ret)
            break;
    }
    free(cmd);

    return ret;
}

static int oxadmin_erase_blk_task ()
{
    int ch_i, lun, blk, ret;
    struct nvm_channel *ch;
    struct nvm_ppa_addr ppa;

    printf("\n Specific block erasing.");
    printf("\n  Specify the channel: ");
    scanf("%d", &ch_i);
    printf("  Specify the LUN: ");
    scanf("%d", &lun);
    printf("  Specify the block: ");
    scanf("%d", &blk);

    if (ch_i < 0 || ch_i >= core.nvm_ch_count)
        goto ERR;

    ch = core.nvm_ch[ch_i];

    if (lun < 0 || lun > ch->geometry->lun_per_ch - 1 ||
                                    blk < 0 || blk > ch->geometry->blk_per_lun)
        goto ERR;

    ppa.g.ch = ch->ch_mmgr_id;
    ppa.g.lun = lun;
    ppa.g.blk = blk;

    if (oxadmin_confirm_erase (1, &ppa)) {
        printf("\n Erase CANCELED.\n");
        return -1;
    }

    printf("\n Erasing...\n");

    ret = oxadmin_erase_blk (ch, &ppa);
    if (ret) {
        printf(" Erase FAILED.\n");
        return -1;
    }

    printf(" 1 block erased succesfully.\n");
    printf("  [ch: %d, lun: %d, blk: %d]\n\n",ch_i,lun,blk);

    return 0;
ERR:
    printf(" Block address out of bounds.\n");
    return -1;
}

static int oxadmin_create_bbt (int type, struct nvm_channel *ch)
{
    struct lnvm_channel *lch;
    struct lnvm_bbtbl *bbt;
    uint32_t tblks;
    int n_pl, ret;

    n_pl = ch->geometry->n_of_planes;

    lch = malloc (sizeof(struct lnvm_channel));
    if (!lch)
        return -1;

    tblks = ch->geometry->blk_per_lun * ch->geometry->lun_per_ch * n_pl;

    lch->ch = ch;

    lch->bbtbl = malloc (sizeof(struct lnvm_bbtbl));
    if (!lch->bbtbl)
        goto FREE_LCH;

    bbt = lch->bbtbl;
    bbt->tbl = malloc (sizeof(uint8_t) * tblks);
    if (!bbt->tbl)
        goto FREE_BBTBL;

    memset (bbt->tbl, 0, tblks);
    bbt->magic = 0;
    bbt->bb_sz = tblks;

    printf("\n [lnvm: Channel %d. Creating bad block table...]", ch->ch_id);
    ret = lnvm_bbt_create (lch, bbt, type);
    if (ret) goto ERR;
    ret = lnvm_flush_bbt (lch, bbt);

ERR:
    free(bbt->tbl);
FREE_BBTBL:
    free(bbt);
FREE_LCH:
    free(lch);

    return ret;
}

static int oxadmin_confirm_bbt_creation (int type, int ch_i)
{
    char choice;

    printf("\n You are about to create/update the bad block table on "
                                                        "channel %d.\n", ch_i);
    printf("\n  Creation type: ");
    switch (type) {
        case LNVM_BBT_FULL:
            printf ("Full scan (ALL DATA WILL BE LOST. "
                                        "FULL SCAN MIGHT TAKE A LONG TIME)\n");
            break;
        case LNVM_BBT_ERASE:
            printf ("Fast scan (ALL DATA WILL BE LOST)\n");
            break;
        case LNVM_BBT_EMERGENCY:
            printf ("Emergency table (ALL BLOCKS WILL BE SET AS GOOD)\n");
            break;
        default:
            return -1;
    }

    printf("\n Are you sure? (Y or N): ");
    scanf (" %c", &choice);

    if (choice == 0x59 || choice == 0x79)
        return 0;

    return -1;
}

static int oxadmin_create_bbt_task ()
{
    int type, ch_i;
    struct nvm_channel *ch;

    printf("\n Bad Block Table creation/update.");
    printf("\n  Bad block creation types: ");
    printf("\n   1 - Full scan (Erase, write full, read full, compare buffers)");
    printf("\n   2 - Fast scan (Only erase the blocks)");
    printf("\n   3 - Emergency table (Creates an empty bad block "
                                            "table without erasing blocks)\n");
    printf("\n  Specify the type: ");
    scanf("%d", &type);

    if (type < 1 || type > 3)
        goto ERR_T;

    printf("  Specify the channel: ");
    scanf("%d", &ch_i);

    if (ch_i < 0 || ch_i >= core.nvm_ch_count)
        goto ERR_CH;

    ch = core.nvm_ch[ch_i];

    switch (type) {
        case 1:
            type = LNVM_BBT_FULL;
            break;
        case 2:
            type = LNVM_BBT_ERASE;
            break;
        case 3:
            type = LNVM_BBT_EMERGENCY;
            break;
        default:
            goto ERR_T;
    }
    if (oxadmin_confirm_bbt_creation (type, ch_i))
        goto ERR_CONF;

    if (oxadmin_create_bbt (type, ch))
        goto ERR_CR;

    printf ("\n  Bad block table created SUCCESFULLY on channel %d.\n", ch_i);
    return 0;

ERR_T:
    printf("\n Incorrect bad block creation type.\n");
    return -1;
ERR_CH:
    printf("\n Channel out of bounds.\n");
    return -1;
ERR_CONF:
    printf("\n Bad block table creation CANCELED.\n");
    return -1;
ERR_CR:
    printf("\n Bad block table creation FAILED.\n");
    return -1;
}

int ox_admin_init (struct nvm_init_arg *args) {
    printf("\n OX Controller ADMIN\n");

    /* List all available admin tasks */
    if (args->arg_flag & CMDARG_FLAG_L) {
        oxadmin_show_all ();
        return 0;
    }

    /* Run a specific admin task */
    if (args->arg_flag & CMDARG_FLAG_T) {
        if (!args->admin_task || strlen(args->admin_task) > CMDARG_LEN) {
            printf(" Wrong admin task name.\n");
            return -1;
        }
        if (strcmp(args->admin_task, "erase-blk") == 0)
            return oxadmin_erase_blk_task ();
        if (strcmp(args->admin_task, "create-bbt") == 0) {
#ifdef CONFIG_FTL_LNVM
            return oxadmin_create_bbt_task ();
#else
            printf (" LightNVM FTL is not compiled.");
            return -1;
#endif
        }
    }

    return -1;
}