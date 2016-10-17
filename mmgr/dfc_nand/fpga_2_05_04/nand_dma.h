/* This file has been modified from the original as follow:
  - Added lines 15, 42, 109, 110 and 111;
  - Number of blocks modified on line 25;
 */

#ifndef __NAND_DMA__
#define __NAND_DMA__

#include<stdint.h>
#include<stdlib.h>
#include<string.h>
#include<time.h>
#include<sys/time.h>
#include"../../include/uatomic.h"
#include "dfc_nand.h"

#define TBL_COUNT		8
#define DESC_PER_TBL		8
#define DESC_SIZE		0x40
#define NAND_CSR_OFFSET		0x10
#define NAND_TBL_OFFSET 	0x800
#define TBL_SZ_SHIFT		0x1
#define TABLE_SHIFT		0x800

#define NAND_BLOCK_COUNT	1024
#define NAND_PLANE_COUNT	2
#define NAND_TARGET_COUNT	2
#define NAND_LUN_COUNT		2
#define NAND_CHIP_COUNT		8 /* 4 channels per DIMM */
#define NAND_PAGE_SIZE		0x4000

#define SUCCESS 			0
#define FAILURE 			-1

#define TOTAL_DESC			(TBL_COUNT * DESC_PER_TBL)
#define CIRC_INCR(idx, limit) (idx = (idx + 1) % limit)

#define SET_LEN_TRGT(len,trgt)	((len<<16) | trgt)

struct timezone zone;

struct dfcnand_io;

void *io_completer();
struct list_head* list_head_bad;

unsigned long long int phy_addr[70];
uint8_t *virt_addr[70];

enum NAND_COMMAND_ID {
    PAGE_PROG           = 0xA,
    PAGE_READ           = 0x1E,
    RESET               = 0x1,
    READ_STATUS         = 0x14,
    BLOCK_ERASE         = 0x5,
    CHANGE_WRITE_COL    = 0xC,
    SET_FEATURE         = 0x9,
};

enum {
    READ_IO     = 1,
    READ_OOB    = 2,
    WRITE       = 3,
    READ_STS    = 4,
    BAD_BLK     = 5,
    OTHERS      = 6,
    ERASE       = 7,
    END_DESC    = 8,
};

enum DESC_TRACK_STAT {
    TRACK_IDX_FREE  = 0,
    TRACK_IDX_ALLOC = 1,
    TRACK_IDX_USED  = 2,
};

typedef struct nand_cmd_struct {
    uint32_t row_addr;
    uint32_t trgt_col_addr;
    uint32_t db_LSB_0;
    uint32_t db_MSB_0;
    uint32_t db_LSB_1;
    uint32_t db_MSB_1;
    uint32_t db_LSB_2;
    uint32_t db_MSB_2;
    uint32_t db_LSB_3;
    uint32_t db_MSB_3;
    uint32_t len1_len0;
    uint32_t len3_len2;
    uint32_t oob_LSB;
    uint32_t oob_len_MSB; /*contains MSB of oob in 1st 20 bits and oob-len
                                                              in next 12 bits*/
    uint32_t buffer_channel_id; /*unused*/
    uint32_t control_fields;
}nand_cmd_struct;

typedef struct io_cmd {
    uint8_t                  chip;
    uint8_t                  target;
    uint8_t                  lun;
    uint16_t                 block;
    uint16_t                 page;
    uint8_t                  status;
    uint8_t                  mul_plane;
    uint16_t                 len[5];
    uint16_t                 col_addr;
    uint64_t                 host_addr[5];

    /* DFC Specific */
    struct dfcnand_io        dfc_io;
    struct nand_cmd_struct   nand_cmd;
} io_cmd;

typedef struct table_size_reg {
    uint16_t table_size;
    uint32_t rsvd:24;
} tblsz_reg;

typedef struct DescStatus {
    uint8_t             *ptr;
    uint8_t 		req_type;
    uint8_t 		page_idx;
    void		*req_ptr;
    uint64_t            page_phy_addr;
    uint8_t             *page_virt_addr;
    pthread_mutex_t     available;
    volatile uint8_t    valid;
} DescStat;

typedef struct nand_descriptor {
    uint32_t row_addr;
    uint32_t column_addr:29;
    uint32_t target:3;
    uint32_t data_buff0_LSB;
    uint32_t data_buff0_MSB;
    uint32_t data_buff1_LSB;
    uint32_t data_buff1_MSB;
    uint32_t data_buff2_LSB;
    uint32_t data_buff2_MSB;
    uint32_t data_buff3_LSB;
    uint32_t data_buff3_MSB;
    uint32_t length0:16;
    uint32_t length1:16;
    uint32_t length2:16;
    uint32_t length3:16;
    uint32_t oob_data_LSB;
    uint32_t oob_data_MSB:20;
    uint32_t oob_length:12;
    uint32_t chn_buff_id;
    uint32_t dir:1;
    uint32_t no_data:1;
    uint32_t ecc:1;
    uint32_t command:6;
    uint32_t irq_en:1;
    uint32_t hold:1;
    uint32_t dma_cmp:1;
    uint32_t desc_id:8;
    uint32_t OwnedByfpga:1;
    uint32_t rdy_busy:1;
    uint32_t chng_pln:1;
    uint32_t rsvd:9;
} __attribute__((packed)) nand_descriptor;

typedef struct csf_reg {
    uint32_t     dir:1;
    uint32_t     no_data:1;
    uint32_t     ecc:1;
    uint32_t     command:6;
    uint32_t     irq_en:1;
    uint32_t     hold:1;
    uint32_t     dma_cmp:1;
    uint32_t     desc_id:8;
    uint32_t     OwnedByFpga:1;
    uint32_t     rsvd:11;
} __attribute__((packed)) csf_reg;

typedef struct csr_reg {
    uint8_t     desc_id;
    uint8_t     err_code:3;
    uint8_t     start:1;
    uint8_t     loop:1;
    uint8_t     reset:1;
    uint32_t    rsvd:18;
} csr_reg;


typedef struct DmaControl {
    DescStat            DescSt[64];
    uint16_t            head_idx;
    uint16_t            tail_idx;
}Dma_Control;

typedef struct Desc_Track {
    DescStat    *DescSt_ptr;
    uint8_t     is_used;
} Desc_Track;

static inline int mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t
                                                                    *mutexattr)
{
    return pthread_mutex_init(mutex, mutexattr);
}

static inline int mutex_destroy(pthread_mutex_t *mutex)
{
    return pthread_mutex_destroy(mutex);
}

static inline int mutex_lock(pthread_mutex_t *mutex)
{
    return pthread_mutex_lock(mutex);
}

static inline int mutex_trylock(pthread_mutex_t *mutex)
{
    return pthread_mutex_trylock(mutex);
}

static inline int mutex_unlock(pthread_mutex_t *mutex)
{
    return pthread_mutex_unlock(mutex);
}

typedef struct  blk_status {
    uint8_t is_bb;		/*Bad block indication byte*/
    uint64_t erase_cnt;		/*Erase count*/
} blk_status_t;

typedef struct oob_data {
    uint64_t lpa;		/*Logical Page Address*/
    uint64_t lpa_usage_cnt;	/*LPA updation count*/
} oob_data_t;

typedef struct nvm_mmgr_ftl {
    uint8_t channel;
    uint16_t block;
    uint16_t page;
} nvm_ftl_t;

typedef struct PgeBlk_info {
    oob_data_t oob_data;	/*Spare area data's*/
    nvm_ftl_t ppa;		/*Physical address of the spare area*/
}PgeBlk_info_t;

uint8_t make_desc(nand_cmd_struct *cmd_struct, uint8_t tid, uint8_t req_type,
                                                                void *req_ptr);
int nand_page_prog ( io_cmd *cmd_buf);
int nand_page_read( io_cmd *cmd_buf);
int nand_block_erase (io_cmd *cmd_buf);
int nand_reset ();
int dfcnand_callback(io_cmd *cmd);
uint8_t nand_dm_deinit ();

#endif
