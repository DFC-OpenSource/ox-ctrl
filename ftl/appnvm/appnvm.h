/* OX: Open-Channel NVM Express SSD Controller
 *  - AppNVM Flash Translation Layer (Header definition and interfaces)
 *
 * Copyright 2018 IT University of Copenhagen.
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
 *
 * Written by Ivan Luiz Picoli <ivpi@itu.dk>
 *
 * Partially supported by CAPES Foundation, Ministry of Education
 * of Brazil, Brasilia - DF 70040-020, Brazil.
 */

#ifndef APPNVM_H
#define APPNVM_H

#include <sys/queue.h>
#include "../../include/ssd.h"
#include "../../include/uatomic.h"

/* ------- GENERAL USE ------- */

#define APP_IO_RETRY       0

#define APP_RSV_BBT_OFF    0
#define APP_RSV_META_OFF   1
#define APP_RSV_MAP_OFF    2
#define APP_RSV_BLK_COUNT  APP_RSV_BBT_OFF + APP_RSV_META_OFF + APP_RSV_MAP_OFF;

#define APP_MAGIC          0x3c

#define APP_TRANS_TO_NVM    0
#define APP_TRANS_FROM_NVM  1

#define APP_IO_NORMAL   0
#define APP_IO_RESERVED 1 /* Used for FTL reserved blocks */

#define APP_INVALID_SECTOR 0
#define APP_INVALID_PAGE   1

enum app_flags_ids {
    APP_FLAGS_ACT_CH   = 1,
    APP_FLAGS_CHECK_GC = 2
};

enum {
    FTL_PGMAP_OFF   = 0,
    FTL_PGMAP_ON    = 1
};

/* ------- FTL CONFIGURATION ------- */

#define APPNVM_FLUSH_RETRY          3
#define APPNVM_GC_THRESD            0.75
#define APPNVM_GC_TARGET_RATE       0.9
#define APPNVM_GC_MAX_BLKS          25
#define APPNVM_GC_MIN_FREE_BLKS     16
#define APPNVM_GC_OVERPROV          0.25

/* ------- MODULARIZED DEBUG ------- */

#define APPNVM_DEBUG_CH_PROV    0
#define APPNVM_DEBUG_GL_PROV    0
#define APPNVM_DEBUG_GC         1

/* ------- APPNVM FTL RELATED ------- */

enum app_bbt_state {
    NVM_BBT_FREE = 0x0, // Block is free AKA good
    NVM_BBT_BAD  = 0x1, // Block is bad
    NVM_BBT_GBAD = 0x2, // Block has grown bad
    NVM_BBT_DMRK = 0x4, // Block has been marked by device side
    NVM_BBT_HMRK = 0x8  // Block has been marked by host side
};

#define APP_BBT_EMERGENCY   0x0  // Creates the bbt without erasing the channel
#define APP_BBT_ERASE       0x1  // Checks for bad blocks only erasing the block
#define APP_BBT_FULL        0x2  // Checks for bad blocks erasing the block,
                                 //   writing and reading all pages,
                                 //   and comparing the buffers

enum app_blk_md_flags {
    APP_BLK_MD_USED = (1 << 0),
    APP_BLK_MD_OPEN = (1 << 1),
    APP_BLK_MD_LINE = (1 << 2),
    APP_BLK_MD_AVLB = (1 << 3)  /* Available: Good block and not reserved */
};

enum app_pg_type {
    APP_PG_RESERVED  = 0x0,
    APP_PG_NAMESPACE = 0x1,
    APP_PG_MAP       = 0x2,
    APP_PG_PADDING   = 0x3
};

/* ------- APPNVM MODULE IDS ------- */

#define APPNVM_MOD_COUNT        9
#define APPNVM_FN_SLOTS         32

enum appnvm_mod_types {
    APPMOD_BBT      = 0x0,
    APPMOD_BLK_MD   = 0x1,
    APPMOD_CH_PROV  = 0x2,
    APPMOD_GL_PROV  = 0x3,
    APPMOD_CH_MAP   = 0x4,
    APPMOD_GL_MAP   = 0x5,
    APPMOD_PPA_IO   = 0x6,
    APPMOD_LBA_IO   = 0x7,
    APPMOD_GC       = 0x8
};

/* Bad block table modules */
#define APPFTL_BBT      0x1

/* Block meta-data modules */
#define APPFTL_BLK_MD   0x1

/* Channel provisioning modules */
#define APPFTL_CH_PROV  0x1

/* Global provisioning modules */
#define APPFTL_GL_PROV  0x1

/* Channel mapping modules */
#define APPFTL_CH_MAP   0x1

/* Global mapping modules */
#define APPFTL_GL_MAP   0x1

/* Back-end PPA I/O modules */
#define APPFTL_PPA_IO   0x1

/* Front-end LBA I/O modules */
#define APPFTL_LBA_IO   0x1

/* Garbage Collection modules */
#define APPFTL_GC       0x1

enum app_gl_functions {
    APP_FN_GLOBAL   = 0
};

/* ------- APPNVM MEMORY STRUCTS ------- */

struct app_l2p_entry {
    uint64_t laddr;
    uint64_t paddr;
};

struct app_magic {
    uint8_t magic;
} __attribute__((packed)) ;

struct app_bbtbl {
    uint8_t  magic;
    uint32_t bb_sz;
    uint32_t bb_count;
    /* This struct is stored on NVM up to this point, *tbl is not stored */
    uint8_t  *tbl;
};

struct app_pg_oob {
    uint64_t    lba;
    uint8_t     pg_type;
    uint8_t     rsv[7];
} __attribute__((packed));

struct app_io_data {
    struct app_channel *lch;
    struct nvm_channel *ch;
    uint8_t             n_pl;
    uint32_t            pg_sz;
    uint8_t            *buf;
    uint8_t           **pl_vec;  /* Array of plane data (first sector) */
    uint8_t           **oob_vec; /* Array of OOB area (per sector) */
    uint8_t          ***sec_vec; /* Array of sectors + OOB [plane][sector] */
    uint32_t            meta_sz;
    uint32_t            buf_sz;
    uint8_t            *mod_oob; /* OOB as SGL can be buffered here */
};

struct app_blk_md_entry {
    uint16_t                flags;
    struct nvm_ppa_addr     ppa;
    uint32_t                erase_count;
    uint16_t                current_pg;
    uint16_t                invalid_sec;
    uint8_t                 pg_state[1024]; /* max of 8192 sectors per block */
} __attribute__((packed));   /* 1042 bytes per entry */

struct app_blk_md {
    uint8_t  magic;
    uint32_t entries;
    size_t   entry_sz;
    /* This struct is stored on NVM up to this point, *tbl is not stored */
    uint8_t  *tbl;
};

struct app_prov_ppas {
    struct app_channel  **ch;
    struct nvm_ppa_addr *ppa;
    uint16_t            nppas;
    uint16_t            nch;
};

struct app_ch_flags {
    uint8_t             active;
    uint8_t             need_gc;
    atomic_t          busy; /* Number of concurrent threads using the ch */
    pthread_spinlock_t  active_spin;
    pthread_spinlock_t  need_gc_spin;
    pthread_spinlock_t  busy_spin;
};

struct app_map_entry {
    uint64_t lba;
    uint64_t ppa;
} __attribute__((packed)); /* 16 bytes entry */

struct app_map_md {
    uint8_t  magic;
    uint32_t entries;
    size_t   entry_sz;
    uint8_t  *tbl;
    pthread_mutex_t *entry_mutex;
};

struct app_channel {
    uint16_t                app_ch_id;
    struct app_ch_flags     flags;
    struct nvm_channel      *ch;
    struct app_bbtbl        *bbtbl;
    struct app_blk_md       *blk_md;
    void                    *ch_prov;
    struct app_map_md       *map_md;
    uint16_t                bbt_blk;  /* Rsvd blk ID for bad block table */
    uint16_t                meta_blk; /* Rsvd blk ID for block metadata */
    uint16_t                map_blk;  /* Rsvd blk ID for mapping metadata */
    LIST_ENTRY(app_channel) entry;
};

/* ------- APPNVM MODULE FUNCTIONS DEFITION ------- */

typedef int                 (app_ch_init)(struct nvm_channel *, uint16_t);
typedef void                (app_ch_exit)(struct app_channel *);
typedef struct app_channel *(app_ch_get)(uint16_t);
typedef int                 (app_ch_get_list)(struct app_channel **, uint16_t);

typedef int      (app_bbt_create)(struct app_channel *, uint8_t);
typedef int      (app_bbt_flush) (struct app_channel *);
typedef int      (app_bbt_load) (struct app_channel *);
typedef uint8_t *(app_bbt_get) (struct app_channel *, uint16_t);

typedef int                      (app_md_create)(struct app_channel *);
typedef int                      (app_md_flush) (struct app_channel *);
typedef int                      (app_md_load) (struct app_channel *);
typedef struct app_blk_md_entry *(app_md_get) (struct app_channel *, uint16_t);
typedef void                     (app_md_invalidate)(struct app_channel *,
                                          struct nvm_ppa_addr *, uint8_t full);

typedef int  (app_ch_prov_init) (struct app_channel *);
typedef void (app_ch_prov_exit) (struct app_channel *);
typedef void (app_ch_prov_check_gc) (struct app_channel *);
typedef int  (app_ch_prov_put_blk) (struct app_channel *, uint16_t, uint16_t);
typedef struct app_blk_md_entry *(app_ch_prov_get_blk) (struct app_channel *,
                                                                     uint16_t);
typedef int  (app_ch_prov_get_ppas)(struct app_channel *, struct nvm_ppa_addr *,
                                                                     uint16_t);

typedef int                   (app_gl_prov_init) (void);
typedef void                  (app_gl_prov_exit) (void);
typedef struct app_prov_ppas *(app_gl_prov_new) (uint32_t);
typedef void                  (app_gl_prov_free) (struct app_prov_ppas *);

typedef int  (app_ch_map_create) (struct app_channel *);
typedef int  (app_ch_map_load) (struct app_channel *);
typedef int  (app_ch_map_flush) (struct app_channel *);
typedef struct app_map_entry *(app_ch_map_get) (struct app_channel *, uint32_t);

typedef int         (app_gl_map_init) (void);
typedef void        (app_gl_map_exit) (void);
typedef int         (app_gl_map_upsert) (uint64_t lba, uint64_t ppa);
typedef uint64_t    (app_gl_map_read) (uint64_t lba);
typedef int         (app_gl_map_upsert_md) (uint64_t index, uint64_t new_ppa,
                                                              uint64_t old_ppa);

typedef int  (app_ppa_io_submit) (struct nvm_io_cmd *);
typedef void (app_ppa_io_callback) (struct nvm_mmgr_io_cmd *);

typedef int  (app_lba_io_init) (void);
typedef void (app_lba_io_exit) (void);
typedef int  (app_lba_io_submit) (struct nvm_io_cmd *);
typedef void (app_lba_io_callback) (struct nvm_io_cmd *);

typedef int                       (app_gc_init) (void);
typedef void                      (app_gc_exit) (void);
typedef struct app_blk_md_entry **(app_gc_target) (struct app_channel *,
                                                                    uint32_t *);
typedef int (app_gc_recycle_blk)(struct app_channel *,struct app_blk_md_entry *,
                                                uint16_t tid, uint32_t *failed);

struct app_channels {
    app_ch_init         *init_fn;
    app_ch_exit         *exit_fn;
    app_ch_get          *get_fn;
    app_ch_get_list     *get_list_fn;
};

struct app_global_bbt {
    uint8_t              mod_id;
    app_bbt_create      *create_fn;
    app_bbt_flush       *flush_fn;
    app_bbt_load        *load_fn;
    app_bbt_get         *get_fn;
};

struct app_global_md {
    uint8_t             mod_id;
    app_md_create      *create_fn;
    app_md_flush       *flush_fn;
    app_md_load        *load_fn;
    app_md_get         *get_fn;
    app_md_invalidate  *invalidate_fn;
};

struct app_ch_prov {
    uint8_t                  mod_id;
    app_ch_prov_init        *init_fn;
    app_ch_prov_exit        *exit_fn;
    app_ch_prov_check_gc    *check_gc_fn;
    app_ch_prov_put_blk     *put_blk_fn;
    app_ch_prov_get_blk     *get_blk_fn;
    app_ch_prov_get_ppas    *get_ppas_fn;
};

struct app_gl_prov {
    uint8_t              mod_id;
    app_gl_prov_init    *init_fn;
    app_gl_prov_exit    *exit_fn;
    app_gl_prov_new     *new_fn;
    app_gl_prov_free    *free_fn;
};

struct app_ch_map {
    uint8_t              mod_id;
    app_ch_map_create   *create_fn;
    app_ch_map_load     *load_fn;
    app_ch_map_flush    *flush_fn;
    app_ch_map_get      *get_fn;
};

struct app_gl_map {
    uint8_t               mod_id;
    app_gl_map_init      *init_fn;
    app_gl_map_exit      *exit_fn;
    app_gl_map_upsert_md *upsert_md_fn;
    app_gl_map_upsert    *upsert_fn;
    app_gl_map_read      *read_fn;
};

struct app_ppa_io {
    uint8_t              mod_id;
    app_ppa_io_submit   *submit_fn;
    app_ppa_io_callback *callback_fn;
};

struct app_lba_io {
    uint8_t              mod_id;
    app_lba_io_init     *init_fn;
    app_lba_io_exit     *exit_fn;
    app_lba_io_submit   *submit_fn;
    app_lba_io_callback *callback_fn;
};

struct app_gc {
    uint8_t              mod_id;
    app_gc_init         *init_fn;
    app_gc_exit         *exit_fn;
    app_gc_target       *target_fn;
    app_gc_recycle_blk  *recycle_fn;
};

struct app_global {
    struct app_channels     channels;

    void                    *mod_list[APPNVM_MOD_COUNT][APPNVM_FN_SLOTS];
    struct app_global_bbt   *bbt;
    struct app_global_md    *md;
    struct app_ch_prov      *ch_prov;
    struct app_gl_prov      *gl_prov;
    struct app_ch_map       *ch_map;
    struct app_gl_map       *gl_map;
    struct app_ppa_io       *ppa_io;
    struct app_lba_io       *lba_io;
    struct app_gc           *gc;
};

/* ------- INLINE FUNCTIONS ------- */

static inline int appnvm_ch_active (struct app_channel *lch)
{
    return lch->flags.active;
}

static inline void appnvm_ch_active_set (struct app_channel *lch)
{
    pthread_spin_lock (&lch->flags.active_spin);
    lch->flags.active = 1;
    pthread_spin_unlock (&lch->flags.active_spin);
}

static inline void appnvm_ch_active_unset (struct app_channel *lch)
{
    pthread_spin_lock (&lch->flags.active_spin);
    lch->flags.active = 0;
    pthread_spin_unlock (&lch->flags.active_spin);
}

static inline int appnvm_ch_need_gc (struct app_channel *lch)
{
    return lch->flags.need_gc;
}

static inline void appnvm_ch_need_gc_set (struct app_channel *lch)
{
    pthread_spin_lock (&lch->flags.need_gc_spin);
    lch->flags.need_gc = 1;
    pthread_spin_unlock (&lch->flags.need_gc_spin);
}

static inline void appnvm_ch_need_gc_unset (struct app_channel *lch)
{
    pthread_spin_lock (&lch->flags.need_gc_spin);
    lch->flags.need_gc = 0;
    pthread_spin_unlock (&lch->flags.need_gc_spin);
}

static inline int appnvm_ch_nthreads (struct app_channel *lch)
{
    int n;

    pthread_spin_lock (&lch->flags.busy_spin);
    n = atomic_read (&lch->flags.busy);
    pthread_spin_unlock (&lch->flags.busy_spin);

    return n;
}

static inline void appnvm_ch_inc_thread (struct app_channel *lch)
{
    int n;

    pthread_spin_lock (&lch->flags.busy_spin);
    n = atomic_read (&lch->flags.busy);
    atomic_set(&lch->flags.busy, ++n);
    pthread_spin_unlock (&lch->flags.busy_spin);
}

static inline void appnvm_ch_dec_thread (struct app_channel *lch)
{
    int n;

    pthread_spin_lock (&lch->flags.busy_spin);
    n = atomic_read (&lch->flags.busy);
    if (n > 0)
        atomic_set(&lch->flags.busy, --n);
    pthread_spin_unlock (&lch->flags.busy_spin);
}

/* ------- GENERAL USE FUNCTIONS ------- */

struct  app_io_data *app_alloc_pg_io (struct app_channel *lch);
void    app_pg_io_prepare (struct app_channel *lch, struct app_io_data *data);
void    app_free_pg_io (struct app_io_data *data);
int     app_io_rsv_blk (struct app_channel *lch, uint8_t cmdtype,
                                     void **buf_vec, uint16_t blk, uint16_t pg);
int     app_pg_io (struct app_channel *lch, uint8_t cmdtype,
                                      void **buf_vec, struct nvm_ppa_addr *ppa);
int     app_blk_current_page (struct app_channel *lch,
                      struct app_io_data *io, uint16_t blk_id, uint16_t offset);
int     app_nvm_seq_transfer (struct app_io_data *io, struct nvm_ppa_addr *ppa,
        uint8_t *user_buf, uint16_t pgs, uint16_t ent_per_pg, uint32_t ent_left,
        size_t entry_sz, uint8_t direction, uint8_t reserved);
int     app_get_ch_list (struct app_channel **list);

/* ------- APPNVM CORE FUNCTIONS ------- */

struct app_global *appnvm (void);
int                appnvm_mod_register(uint8_t modtype, uint8_t id, void *mod);
int                appnvm_mod_set (uint8_t *modset);

/* ------- APPNVM MODULES REGISTRATION ------- */

void channels_register (void);
void bbt_byte_register (void);
void blk_md_register (void);
void ch_prov_register (void);
void gl_prov_register (void);
void ch_map_register (void);
void gl_map_register (void);
void ppa_io_register (void);
void lba_io_register (void);
void gc_register (void);

#endif /* APP_H */