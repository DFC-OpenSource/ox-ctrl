#ifndef LIGHTNVM_H
#define LIGHTNVM_H

#include <stdint.h>
#include <stdio.h>

#define LNVM_SECSZ          0x1000
#define LNVM_SEC_OOBSZ      0x100
#define LNVM_SEC_PG         4
#define LNVM_PG_BLK         512
#define LNVM_CH             8
#define LNVM_LUN_CH         4
#define LNVM_PLANES         2
#define LNVM_PG_SIZE        (LNVM_SECSZ * LNVM_SEC_PG)
#define LNVM_PL_PG_SIZE     (LNVM_SECSZ * LNVM_SEC_PG * LNVM_PLANES)
#define LNVM_SEC_PL_PG      (LNVM_SEC_PG * LNVM_PLANES)

#define LNVM_MAX_SEC_RQ     64
#define LNVM_MTYPE          0
#define LNVM_FMTYPE         0
#define LNVM_VER_ID         1
#define LNVM_DOM            0x0
#define LNVM_CAP            0x3
#define LNVM_READ_L2P       0x1
#define LNVM_BB_GEN_FREQ    0x0
#define LNVM_ERR_WRITE      0x0
#define LNVM_VMNT       0
#define LNVM_CGRPS      1

#define LNVM_TRDT           1600
#define LNVM_TRDM           1600
#define LNVM_TPRT           800
#define LNVM_TPRM           800
#define LNVM_TBET           2400
#define LNVM_TBEM           2400

#define LNVM_MAX_GRPS_PR_IDENT (20)
#define LNVM_FEAT_EXT_START 64
#define LNVM_FEAT_EXT_END 127
#define LNVM_PBA_UNMAPPED UINT64_MAX
#define LNVM_LBA_UNMAPPED UINT64_MAX

enum LnvmAdminCommands {
    LNVM_ADM_CMD_IDENTITY           = 0xe2,
    LNVM_ADM_CMD_GET_L2P_TBL        = 0xea,
    LNVM_ADM_CMD_GET_BB_TBL         = 0xf2,
    LNVM_ADM_CMD_SET_BB_TBL         = 0xf1,
};

enum LnvmDmCommands {
    LNVM_CMD_HYBRID_WRITE      = 0x81,
    LNVM_CMD_HYBRID_READ       = 0x02,
    LNVM_CMD_PHYS_WRITE        = 0x91,
    LNVM_CMD_PHYS_READ         = 0x92,
    LNVM_CMD_ERASE_SYNC        = 0x90,
};

typedef struct LnvmIdAddrFormat {
    uint8_t  ch_offset;
    uint8_t  ch_len;
    uint8_t  lun_offset;
    uint8_t  lun_len;
    uint8_t  pln_offset;
    uint8_t  pln_len;
    uint8_t  blk_offset;
    uint8_t  blk_len;
    uint8_t  pg_offset;
    uint8_t  pg_len;
    uint8_t  sect_offset;
    uint8_t  sect_len;
    uint8_t  res[4];
} LnvmIdAddrFormat;

typedef struct LnvmIdGroup {
    uint8_t    mtype;
    uint8_t    fmtype;
    uint16_t   res16;
    uint8_t    num_ch;
    uint8_t    num_lun;
    uint8_t    num_pln;
    uint8_t    rsvd1;
    uint16_t   num_blk;
    uint16_t   num_pg;
    uint16_t   fpg_sz;
    uint16_t   csecs;
    uint16_t   sos;
    uint16_t   rsvd2;
    uint32_t   trdt;
    uint32_t   trdm;
    uint32_t   tprt;
    uint32_t   tprm;
    uint32_t   tbet;
    uint32_t   tbem;
    uint32_t   mpos;
    uint32_t   mccap;
    uint16_t   cpar;
    uint8_t    res[906];
} LnvmIdGroup;

typedef struct LnvmIdCtrl {
    uint8_t       ver_id;
    uint8_t       vmnt;
    uint8_t       cgrps;
    uint8_t       res;
    uint32_t      cap;
    uint32_t      dom;
    struct LnvmIdAddrFormat ppaf;
    uint8_t       resv[228];
    LnvmIdGroup   groups[4];
} LnvmIdCtrl;

typedef struct LnvmParams {
    /* configurable device characteristics */
    uint16_t    pgs_per_blk;
    uint16_t    sec_size;
    uint8_t     secs_per_pg;
    uint8_t     max_sec_per_rq;
    /* configurable parameters for LnvmIdGroup */
    uint8_t     mtype;
    uint8_t     fmtype;
    uint8_t     num_ch;
    uint8_t     num_pln;
    uint8_t     num_lun;
    /* calculated values */
    uint32_t    sec_per_phys_pl;
    uint32_t    sec_per_log_pl;
    uint32_t    sec_per_blk;
    uint32_t    sec_per_lun;
    uint32_t    total_secs;
} LnvmParams;

typedef struct LnvmGetL2PTbl {
    uint8_t opcode;
    uint8_t flags;
    uint16_t cid;
    uint32_t nsid;
    uint32_t rsvd1[4];
    uint64_t prp1;
    uint64_t prp2;
    uint64_t slba;
    uint32_t nlb;
    uint16_t rsvd2[6];
} LnvmGetL2PTbl;

typedef struct LnvmGetBBTbl {
    uint8_t opcode;
    uint8_t flags;
    uint16_t cid;
    uint32_t nsid;
    uint64_t rsvd1[2];
    uint64_t prp1;
    uint64_t prp2;
    uint64_t spba;
    uint32_t rsvd4[4]; // DW15, 14, 13, 12
} LnvmGetBBTbl;

typedef struct LnvmSetBBTbl {
    uint8_t opcode;
    uint8_t flags;
    uint16_t cid;
    uint32_t nsid;
    uint64_t rsvd1[2];
    uint64_t prp1;
    uint64_t prp2;
    uint64_t spba;
    uint16_t nlb;
    uint8_t value;
    uint8_t rsvd3;
    uint32_t rsvd4[3];
} LnvmSetBBTbl;

typedef struct LnvmBBTbl {
    uint8_t     tblid[4];
    uint16_t    verid;
    uint16_t    revid;
    uint32_t    rvsd1;
    uint32_t    tblks;
    uint32_t    tfact;
    uint32_t    tgrown;
    uint32_t    tdresv;
    uint32_t    thresv;
    uint32_t    rsvd2[8];
    uint8_t     blk[0];
} LnvmBBTbl;

typedef struct LnvmRwCmd {
    uint8_t     opcode;
    uint8_t     flags;
    uint16_t    cid;
    uint32_t    nsid;
    uint64_t    rsvd2;
    uint64_t    metadata;
    uint64_t    prp1;
    uint64_t    prp2;
    uint64_t    spba;
    uint16_t    nlb;
    uint16_t    control;
    uint32_t    dsmgmt;
    uint64_t    slba;
} LnvmRwCmd;

typedef struct LnvmCtrl {
    LnvmParams     params;
    LnvmIdCtrl     id_ctrl;
    uint8_t        bb_gen_freq;
    uint32_t       err_write;
    uint32_t       err_write_cnt;
} LnvmCtrl;

#endif /* LIGHTNVM_H */