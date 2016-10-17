#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../include/tests.h"
#include "../include/ssd.h"

extern struct core_struct core;

static void oxadmin_show_all ()
{
    printf(" \nAvailable OX Admin Tasks: \n");
    printf("\n  - 'erase-blk': erase specific blocks.");
    printf("\n     eg. ox-ctrl admin -t erase-blk\n");
    printf("\n  - 'erase-lun': erase specific LUNs.");
    printf("\n     eg. ox-ctrl admin -t erase-lun (not implemented)\n");
    printf("\n  - 'erase-ch': erase specific channels.");
    printf("\n     eg. ox-ctrl admin -t erase-ch (not implemented)\n\n");
}

static int oxadmin_confirm_erase (int n, struct nvm_ppa_addr *ppa) {
    char choice;
    printf("\n You asked to erase %d blocks.\n", n);
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

    if (ch_i < 0 || ch_i > core.nvm_ch_count)
        goto ERR;

    ch = core.nvm_ch[ch_i];

    if (lun < 0 || lun > ch->geometry->lun_per_ch - 1 ||
                                    blk < 0 || blk > ch->geometry->blk_per_lun)
        goto ERR;

    ppa.g.ch = ch->ch_mmgr_id;
    ppa.g.lun = lun;
    ppa.g.blk = blk;

    if (oxadmin_confirm_erase (1, &ppa)) {
        printf("\n Erased CANCELED.\n");
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

int ox_admin_init (struct nvm_init_arg *args) {
    printf("\n OX Controller ADMIN\n");

    /* List all available admin tasks */
    if (args->arg_flag & CMDARG_FLAG_L) {
        oxadmin_show_all ();
        return -1;
    }

    /* Run a specific admin task */
    if (args->arg_flag & CMDARG_FLAG_T) {
        if (!args->admin_task || strlen(args->admin_task) > CMDARG_LEN) {
            printf(" Wrong admin task name.\n");
            return -1;
        }
        if (strcmp(args->admin_task, "erase-blk") == 0)
            return oxadmin_erase_blk_task ();
    }

    return -1;
}