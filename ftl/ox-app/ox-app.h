/* OX: Open-Channel NVM Express SSD Controller
 *  - AppNVM Flash Translation Layer (Header definition and interfaces)
 *
 * Copyright 2018 IT University of Copenhagen
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

#ifndef APPNVM_H
#define APPNVM_H

#include <sys/queue.h>
#include <libox.h>
#include <ox-uatomic.h>

/* ------- GENERAL USE ------- */

#define APP_IO_RETRY       0

#define APP_MD_IO_RETRY         256
#define APP_MD_IO_RETRY_DELAY   100

#define APP_RSV_BBT_OFF    0
#define APP_RSV_META_OFF   1
#define APP_RSV_MAP_OFF    2
#define APP_RSV_CP_OFF     3

#define APP_RSV_BLK_COUNT  4;

#define APP_MAGIC          0x3c

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

#define APP_FLUSH_RETRY          3
#define APP_GC_THRESD            0.70
#define APP_GC_TARGET_RATE       0.5
#define APP_GC_MAX_BLKS          25
#define APP_GC_MIN_FREE_BLKS     32
#define APP_GC_OVERPROV          0.3

#define APP_CP_INTERVAL          30
#define APP_CP_MIN_INTERVAL      3

/* ------- MODULARIZED DEBUG ------- */

#define APP_DEBUG_CH_PROV    0
#define APP_DEBUG_GL_PROV    0
#define APP_DEBUG_BLK_MD     0
#define APP_DEBUG_CH_MAP     0
#define APP_DEBUG_GL_MAP     0
#define APP_DEBUG_LBA_IO     0
#define APP_DEBUG_LOG        0
#define APP_DEBUG_GC         0
#define APP_DEBUG_RECOVERY   0

/* ------- APPNVM FTL RELATED ------- */

enum app_bbt_state {
    APP_BBT_FREE = 0x0, // Block is free AKA good
    APP_BBT_BAD  = 0x1, // Block is bad
    APP_BBT_GBAD = 0x2, // Block has grown bad
    APP_BBT_DMRK = 0x4, // Block has been marked by device side
    APP_BBT_HMRK = 0x8  // Block has been marked by host side
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
    APP_BLK_MD_AVLB = (1 << 3), /* Available: Good block and not reserved */
    APP_BLK_MD_COLD = (1 << 4), /* Contains cold data recycled by GC */
    APP_BLK_MD_META = (1 << 5)  /* Contains metadata, such as log for recovery*/
};

enum app_pg_type {
    APP_PG_RESERVED  = 0x0,
    APP_PG_NAMESPACE = 0x1c,
    APP_PG_MAP       = 0x2c,
    APP_PG_MAP_MD    = 0x3c,
    APP_PG_BLK_MD    = 0x4c,
    APP_PG_PADDING   = 0x5c,
    APP_PG_LOG       = 0x6c,
    APP_PG_UNKNOWN   = 0xff,
};

enum app_line_type {
    APP_LINE_USER = 0x0, /* Provisioning for user (hot) data */
    APP_LINE_COLD = 0x1, /* Provisioning for GC (cold) data */
    APP_LINE_META = 0x2  /* Provisioning for metadata (recovery log) */
};

enum app_log_type {
    APP_LOG_PAD      = 0x0,
    APP_LOG_POINTER  = 0x1,
    APP_LOG_WRITE    = 0x2,
    APP_LOG_MAP      = 0x3,
    APP_LOG_GC_WRITE = 0x4,
    APP_LOG_GC_MAP   = 0x5,
    APP_LOG_AMEND    = 0x6,
    APP_LOG_COMMIT   = 0x7,
    APP_LOG_BLK_MD   = 0x8,
    APP_LOG_MAP_MD   = 0x9,
    APP_LOG_ABORT_W  = 0xa,     /* Aborted write, used in log management */
    APP_LOG_PUT_BLK  = 0xb
};

enum app_transaction_type {
    APP_TR_LBA_NS   = 0x0,  /* Namespace LBA I/O write */
    APP_TR_GC_NS    = 0x1,  /* Namespace GC write */
    APP_TR_GC_MAP   = 0x2   /* Mapping GC write */
};

enum app_checkpoint_type {
    APP_CP_BLK_MD       = 0x000, /* 0x000 - 0x0ff */
    APP_CP_CH_MAP       = 0x100, /* 0x100 - 0x1ff */
    APP_CP_LOG_TAIL     = 0x200,
    APP_CP_LOG_HEAD     = 0x201,
    APP_CP_GL_PROV_CH   = 0x202,
    APP_CP_TIMESTAMP    = 0x203
};

enum ox_stats_event_code {
    OX_GC_EVENT_BLOCK_REC   = 0x1,
    OX_GC_EVENT_FAILED      = 0x2,
    OX_GC_EVENT_RACE_BIG    = 0x3,
    OX_GC_EVENT_RACE_SMALL  = 0x4,
    OX_GC_EVENT_SPACE_REC   = 0x5,
    OX_CP_EVENT_FLUSH       = 0x6
};
#define APP_T_FLUSH_NO      0
#define APP_T_FLUSH_YES     1

/* ------- OXAPP MODULE IDS ------- */

#define APP_MOD_COUNT        11
#define APP_FN_SLOTS         32

enum appnvm_mod_types {
    APPMOD_BBT      = 0x0,
    APPMOD_BLK_MD   = 0x1,
    APPMOD_CH_PROV  = 0x2,
    APPMOD_GL_PROV  = 0x3,
    APPMOD_CH_MAP   = 0x4,
    APPMOD_GL_MAP   = 0x5,
    APPMOD_PPA_IO   = 0x6,
    APPMOD_LBA_IO   = 0x7,
    APPMOD_GC       = 0x8,
    APPMOD_LOG      = 0x9,
    APPMOD_RECOVERY = 0xa
};

/* Bad block table modules */
#define OXBLK_BBT      0x1

/* Block meta-data modules */
#define OXBLK_BLK_MD   0x1
#define ELEOS_BLK_MD   0x2
#define NR_BLK_MD      0x3

/* Channel provisioning modules */
#define OXBLK_CH_PROV  0x1

/* Global provisioning modules */
#define OXBLK_GL_PROV  0x1

/* Channel mapping modules */
#define OXBLK_CH_MAP   0x1
#define ELEOS_CH_MAP   0x2
#define NR_CH_MAP      0x3

/* Global mapping modules */
#define OXBLK_GL_MAP   0x1

/* Back-end PPA I/O modules */
#define OXBLK_PPA_IO   0x1

/* Front-end LBA I/O modules */
#define OXBLK_LBA_IO   0x1
#define ELEOS_LBA_IO   0x2

/* Garbage Collection modules */
#define OXBLK_GC       0x1

/* Log History modules */
#define OXBLK_LOG      0x1

/* Recovery modules */
#define OXBLK_RECOVERY 0x1

enum app_gl_functions {
    APP_FN_GLOBAL      = 0,
    APP_FN_TRANSACTION = 1
};

/* ------- APPNVM MEMORY STRUCTS ------- */

struct app_l2p_entry {
    uint64_t laddr;
    uint64_t paddr;
};

struct app_magic {
    uint32_t rsvd;
    uint32_t magic;
} __attribute__((packed));

struct app_bbtbl {
    struct nvm_magic byte;
    uint32_t         bb_sz;
    uint32_t         bb_count;
    /* This struct is stored on NVM up to this point, *tbl is not stored */
    uint8_t          *tbl;
} __attribute__((packed));

struct app_sec_oob {
    uint8_t  rsvd1[4];
    uint64_t lba;
    uint8_t  pg_type;
    uint8_t  rsvd2[3];
} __attribute__((packed));

struct app_pg_oob {
    uint64_t    lba;
    uint8_t     pg_type;
    uint8_t     rsv[7];
} __attribute__((packed));

#define BLK_MD_BYTE_VEC_SZ    1024 /* max of 4096 sectors per block */
struct app_blk_md_entry {
    uint16_t                flags;
    struct nvm_ppa_addr     ppa;
    uint32_t                erase_count;
    uint16_t                current_pg;
    uint16_t                invalid_sec;
    uint8_t                 pg_state[BLK_MD_BYTE_VEC_SZ];
} __attribute__((packed));   /* 1042 bytes per entry */

struct app_tiny_entry {
    struct nvm_ppa_addr ppa;
} __attribute__((packed)); /* 8 bytes entry */

struct app_tiny_tbl {
    struct app_tiny_entry *tbl;
    uint8_t               *dirty;
    uint32_t               entries;
    uint32_t               entry_sz;
};

struct app_blk_md {
    struct nvm_magic byte;
    uint32_t         entries;
    uint32_t         entry_sz;
    /* This struct is stored on NVM up to this point */

    uint8_t             *tbl;    /* This is the 'small' fixed table */
    uint32_t             ent_per_pg;
    uint64_t            *closed_ts; /* Timestamps when blocks were closed */
    struct app_tiny_tbl  tiny;   /* This is the 'tiny' table for checkpoint */
    pthread_spinlock_t  *entry_spin;
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
    u_atomic_t          busy; /* Number of concurrent threads using the ch */
    pthread_spinlock_t  active_spin;
    pthread_spinlock_t  need_gc_spin;
    pthread_spinlock_t  busy_spin;
};

struct app_map_entry {
    uint64_t lba;
    uint64_t ppa;
} __attribute__((packed)); /* 16 bytes entry */

struct app_map_md {
    struct nvm_magic byte;
    uint32_t         entries;
    uint32_t         entry_sz;
    /* This struct is stored on NVM up to this point */

    uint8_t             *tbl;
    uint32_t             ent_per_pg;
    struct app_tiny_tbl  tiny;   /* This is the 'tiny' table for checkpoint */

    pthread_mutex_t     *entry_mutex;
    pthread_spinlock_t  *entry_spin;
    pthread_mutex_t     tbl_mutex;  /* Used during table copy (checkpoint) */
} __attribute__((packed));

struct app_log_entry {
    uint64_t ts;
    uint8_t  type;
    uint8_t  rsv[7];

    union {
        uint8_t byte[48];

        struct {
            uint64_t tid;       /* Transaction ID */
            uint64_t lba;
            uint64_t old_ppa;
            uint64_t new_ppa;
            uint8_t  rsv[16];
        } __attribute__((packed)) write;

        struct {
            uint64_t ppa;
            uint64_t old_ppa;
            uint64_t new_ppa;
            uint8_t  prov_type;
            uint8_t  rsv[23];
        } __attribute__((packed)) amend;

        struct {
            uint64_t prev[3];
            uint64_t next[3];
        } __attribute__((packed)) pointer;
    };
} __attribute__((packed));

struct app_transaction_log {
    uint64_t tid;
    uint64_t ts;
    uint64_t lba;
    struct nvm_ppa_addr ppa;
    struct app_transaction_t *tr;
    uint8_t abort;
};

struct app_transaction_pad {
    void                      **pad;
    uint32_t                    count;
    uint32_t                    padded;
    struct app_transaction_log *log;

    /* For internal use */
    struct app_log_entry      **rsv;
};

struct app_transaction_t {
    uint16_t                    tid;
    uint64_t                    ts;
    uint32_t                    count;
    struct app_transaction_log *entries;
    uint32_t                    allocated;
    uint8_t                     committed;
    uint8_t                     flush_commit;
    uint8_t                     tr_type;
    struct nvm_callback        *cb;
    pthread_spinlock_t          spin;
    TAILQ_ENTRY (app_transaction_t) entry;
};

struct app_transaction_user {
    uint16_t                     tid;
    uint64_t                     ts;
    uint32_t                     count;
    struct app_transaction_log **entries;
};

struct app_hmap;
struct app_hmap_entry {
    uint64_t lba;
    uint64_t ppa;
} __attribute__((packed)); /* 16 bytes entry */

struct app_hmap_level {
    uint16_t                level;
    uint64_t                tbl_sz;
    uint32_t                ent_sz;
    uint64_t                n_entries;
    struct app_hmap_entry  *buf;
    uint8_t                *static_flag; /* Used to keep track of dirty pages */
    uint32_t                static_pgs;
    uint8_t                 use_cache;
    struct app_hmap        *hmap;
    struct app_cache       *cache;
};

struct app_hmap {
    uint8_t                  id;
    uint32_t                 ent_per_pg;
    struct app_hmap_level    level[8];
    uint16_t                 n_levels;
    struct nvm_mmgr_geometry *geo;
};

typedef int (app_cache_rw_fn) (uint8_t *buf, struct app_hmap_entry *ent);

struct app_cache_entry {
    uint8_t                     dirty;
    uint8_t                    *buf;
    uint32_t                    buf_sz;
    struct nvm_ppa_addr         ppa;    /* Stores the PPA while pg is cached */
    struct app_hmap_entry      *up_entry;
    struct app_cache           *cache;
    pthread_mutex_t            *mutex;
    pthread_spinlock_t         *spin;
    LIST_ENTRY(app_cache_entry)  f_entry;
    TAILQ_ENTRY(app_cache_entry) u_entry;
};

struct app_cache {
    struct app_cache_entry                 *pg_buf;
    LIST_HEAD(b_free_l, app_cache_entry)   mbf_head;
    TAILQ_HEAD(b_used_l, app_cache_entry)  mbu_head;
    pthread_spinlock_t                      mb_spin;
    uint32_t                                nfree;
    uint32_t                                nused;
    uint16_t                                id;
    uint32_t                                cache_sz;
    uint32_t                                cache_ent_sz;
    uint16_t                                ox_mem_id;
    uint16_t                                instances;
    app_cache_rw_fn                        *read_fn;
    app_cache_rw_fn                        *write_fn;
};

struct app_rec_oob {
    uint32_t rsvd;
    uint32_t magic;
    uint32_t checkpoint_sz;
} __attribute__((packed));

struct app_rec_entry {
    uint32_t  type;
    uint32_t  size;
    void     *data;
    LIST_ENTRY(app_rec_entry) entry;
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
    uint16_t                cp_blk;   /* Rsvd blk ID for checkpoint */
    LIST_ENTRY(app_channel) entry;
};

/* ------- APPNVM MODULE FUNCTIONS DEFINITION ------- */

typedef int                 (app_ch_init)(struct nvm_channel *, uint16_t);
typedef void                (app_ch_exit)(struct app_channel *);
typedef struct app_channel *(app_ch_get)(uint16_t);
typedef int                 (app_ch_get_list)(struct app_channel **, uint16_t);

typedef int      (app_bbt_create)(struct app_channel *, uint8_t);
typedef int      (app_bbt_flush) (struct app_channel *);
typedef int      (app_bbt_load) (struct app_channel *);
typedef uint8_t *(app_bbt_get) (struct app_channel *, uint16_t);

typedef int             (app_md_create)(struct app_channel *);
typedef int             (app_md_flush) (struct app_channel *);
typedef int             (app_md_load) (struct app_channel *);
typedef void            (app_md_mark) (struct app_channel *lch, uint64_t index);
typedef struct app_blk_md_entry *(app_md_get) (struct app_channel *, uint16_t);
typedef void                     (app_md_invalidate)(struct app_channel *,
                                          struct nvm_ppa_addr *, uint8_t full);

typedef int  (app_ch_prov_init) (struct app_channel *);
typedef void (app_ch_prov_exit) (struct app_channel *);
typedef void (app_ch_prov_check_gc) (struct app_channel *);
typedef int  (app_ch_prov_close_blk) (struct app_channel *,
                                        struct nvm_ppa_addr *ppa, uint8_t type);
typedef int  (app_ch_prov_put_blk) (struct app_channel *, uint16_t, uint16_t);
typedef int  (app_ch_prov_get_ppas)(struct app_channel *, struct nvm_ppa_addr *,
                                                            uint16_t,  uint8_t);

typedef int                   (app_gl_prov_init) (void);
typedef void                  (app_gl_prov_exit) (void);
typedef struct app_prov_ppas *(app_gl_prov_new) (uint32_t, uint8_t);
typedef void                  (app_gl_prov_free) (struct app_prov_ppas *);

typedef int  (app_ch_map_create) (struct app_channel *);
typedef int  (app_ch_map_load) (struct app_channel *);
typedef int  (app_ch_map_flush) (struct app_channel *);
typedef void (app_ch_map_mark) (struct app_channel *lch, uint64_t index);
typedef struct app_map_entry *(app_ch_map_get) (struct app_channel *, uint32_t);

typedef int      (app_gl_map_init) (void);
typedef void     (app_gl_map_exit) (void);
typedef void     (app_gl_map_clear) (void);
typedef int      (app_gl_map_upsert)(uint64_t lba, uint64_t ppa, uint64_t *old,
                                                           uint64_t old_caller);
typedef uint64_t (app_gl_map_read) (uint64_t lba);
typedef int      (app_gl_map_upsert_md) (uint64_t index, uint64_t new_ppa,
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

typedef int  (app_log_init) (void);
typedef void (app_log_exit) (void);
typedef void (app_log_truncate) (void);
typedef int  (app_log_append) (struct app_log_entry *list, uint16_t size);
typedef int  (app_log_flush) (uint16_t size, struct app_transaction_pad *pad,
                                                       struct nvm_callback *cb);

typedef int  (app_recovery_init) (void);
typedef void (app_recovery_exit) (void);
typedef int  (app_recovery_checkpoint) (void);
typedef int  (app_recovery_recover) (void);
typedef int  (app_recovery_log_replay) (void);
typedef int  (app_recovery_set) (struct app_rec_entry *);
typedef struct app_rec_entry *(app_recovery_get) (uint32_t type);

struct app_channels {
    app_ch_init         *init_fn;
    app_ch_exit         *exit_fn;
    app_ch_get          *get_fn;
    app_ch_get_list     *get_list_fn;
};

struct app_global_bbt {
    uint8_t              mod_id;
    char                *name;
    app_bbt_create      *create_fn;
    app_bbt_flush       *flush_fn;
    app_bbt_load        *load_fn;
    app_bbt_get         *get_fn;
};

struct app_global_md {
    uint8_t             mod_id;
    char               *name;
    app_md_create      *create_fn;
    app_md_flush       *flush_fn;
    app_md_load        *load_fn;
    app_md_get         *get_fn;
    app_md_invalidate  *invalidate_fn;
    app_md_mark        *mark_fn;
};

struct app_ch_prov {
    uint8_t                  mod_id;
    char                    *name;
    app_ch_prov_init        *init_fn;
    app_ch_prov_exit        *exit_fn;
    app_ch_prov_check_gc    *check_gc_fn;
    app_ch_prov_close_blk   *close_blk_fn;
    app_ch_prov_put_blk     *put_blk_fn;
    app_ch_prov_get_ppas    *get_ppas_fn;
};

struct app_gl_prov {
    uint8_t              mod_id;
    char                *name;
    app_gl_prov_init    *init_fn;
    app_gl_prov_exit    *exit_fn;
    app_gl_prov_new     *new_fn;
    app_gl_prov_free    *free_fn;
};

struct app_ch_map {
    uint8_t              mod_id;
    char                *name;
    app_ch_map_create   *create_fn;
    app_ch_map_load     *load_fn;
    app_ch_map_flush    *flush_fn;
    app_ch_map_get      *get_fn;
    app_ch_map_mark     *mark_fn;
};

struct app_gl_map {
    uint8_t               mod_id;
    char                 *name;
    app_gl_map_init      *init_fn;
    app_gl_map_exit      *exit_fn;
    app_gl_map_clear     *clear_fn;
    app_gl_map_upsert_md *upsert_md_fn;
    app_gl_map_upsert    *upsert_fn;
    app_gl_map_read      *read_fn;
};

struct app_ppa_io {
    uint8_t              mod_id;
    char                *name;
    app_ppa_io_submit   *submit_fn;
    app_ppa_io_callback *callback_fn;
};

struct app_lba_io {
    uint8_t              mod_id;
    char                *name;
    app_lba_io_init     *init_fn;
    app_lba_io_exit     *exit_fn;
    app_lba_io_submit   *submit_fn;
    app_lba_io_callback *callback_fn;
};

struct app_gc {
    uint8_t              mod_id;
    char                *name;
    app_gc_init         *init_fn;
    app_gc_exit         *exit_fn;
    app_gc_target       *target_fn;
    app_gc_recycle_blk  *recycle_fn;
};

struct app_log {
    uint8_t              mod_id;
    char                *name;
    app_log_init        *init_fn;
    app_log_exit        *exit_fn;
    app_log_append      *append_fn;
    app_log_flush       *flush_fn;
    app_log_truncate    *truncate_fn;
};

struct app_recovery {
    uint8_t                  mod_id;
    char                    *name;
    app_recovery_init       *init_fn;
    app_recovery_exit       *exit_fn;
    app_recovery_checkpoint *checkpoint_fn;
    app_recovery_recover    *recover_fn;
    app_recovery_log_replay *log_replay_fn;
    app_recovery_set        *set_fn;
    app_recovery_get        *get_fn;
    uint8_t                  running;
};

struct app_global {
    struct app_channels     channels;

    void                    *mod_list[APP_MOD_COUNT][APP_FN_SLOTS];
    struct app_global_bbt   *bbt;
    struct app_global_md    *md;
    struct app_ch_prov      *ch_prov;
    struct app_gl_prov      *gl_prov;
    struct app_ch_map       *ch_map;
    struct app_gl_map       *gl_map;
    struct app_ppa_io       *ppa_io;
    struct app_lba_io       *lba_io;
    struct app_gc           *gc;
    struct app_log          *log;
    struct app_recovery     *recovery;
};

/* ------- CHANNEL FUNCTIONS ------- */

static inline int app_ch_active (struct app_channel *lch)
{
    return lch->flags.active;
}

static inline void app_ch_active_set (struct app_channel *lch)
{
    pthread_spin_lock (&lch->flags.active_spin);
    lch->flags.active = 1;
    pthread_spin_unlock (&lch->flags.active_spin);
}

static inline void app_ch_active_unset (struct app_channel *lch)
{
    pthread_spin_lock (&lch->flags.active_spin);
    lch->flags.active = 0;
    pthread_spin_unlock (&lch->flags.active_spin);
}

static inline int app_ch_need_gc (struct app_channel *lch)
{
    return lch->flags.need_gc;
}

static inline void app_ch_need_gc_set (struct app_channel *lch)
{
    pthread_spin_lock (&lch->flags.need_gc_spin);
    lch->flags.need_gc = 1;
    pthread_spin_unlock (&lch->flags.need_gc_spin);
}

static inline void app_ch_need_gc_unset (struct app_channel *lch)
{
    pthread_spin_lock (&lch->flags.need_gc_spin);
    lch->flags.need_gc = 0;
    pthread_spin_unlock (&lch->flags.need_gc_spin);
}

static inline int app_ch_nthreads (struct app_channel *lch)
{
    int n;

    pthread_spin_lock (&lch->flags.busy_spin);
    n = u_atomic_read (&lch->flags.busy);
    pthread_spin_unlock (&lch->flags.busy_spin);

    return n;
}

static inline void app_ch_inc_thread (struct app_channel *lch)
{
    int n;

    pthread_spin_lock (&lch->flags.busy_spin);
    n = u_atomic_read (&lch->flags.busy);
    u_atomic_set(&lch->flags.busy, ++n);
    pthread_spin_unlock (&lch->flags.busy_spin);
}

static inline void app_ch_dec_thread (struct app_channel *lch)
{
    int n;

    pthread_spin_lock (&lch->flags.busy_spin);
    n = u_atomic_read (&lch->flags.busy);
    if (n > 0)
        u_atomic_set(&lch->flags.busy, --n);
    pthread_spin_unlock (&lch->flags.busy_spin);
}

int app_get_ch_list (struct app_channel **list);

/* ------- TRANSACTION FUNCTIONS ------- */

int         app_transaction_init (void);
void        app_transaction_exit (void);
int         app_transaction_commit (struct app_transaction_t *tr,
                                        struct nvm_callback *cb, uint8_t flush);
void        app_transaction_abort (struct app_transaction_t *tr);
void        app_transaction_abort_by_ts (uint64_t tid, uint64_t ts);
int         app_transaction_close_blk (struct nvm_ppa_addr *ppa, uint8_t type);
void        app_transaction_free_list (struct app_prov_ppas *prov);
struct app_transaction_t    *app_transaction_new (uint64_t *lbas,uint32_t count,
                            uint8_t tr_type);
struct app_prov_ppas        *app_transaction_amend (
                            struct app_transaction_user *user, uint8_t type);
struct app_prov_ppas        *app_transaction_alloc_list (
                            struct app_transaction_user *ent, uint8_t tr_type);

/* ------- HIERARCHICAL MAPPING FUNCTIONS ------- */

int              app_hmap_init (void);
void             app_hmap_exit (void);
int              app_hmap_build (struct app_hmap *map, uint8_t *tiny);
void             app_hmap_destroy (struct app_hmap *map);
struct app_hmap *app_hmap_create (uint8_t id, uint64_t entries,
                                                 struct nvm_mmgr_geometry *geo);
int         app_hmap_upsert (struct app_hmap *map, uint64_t lba, uint64_t ppa);
uint64_t    app_hmap_get (struct app_hmap *map, uint64_t lba);

/* ------- CACHING FUNCTIONS ------- */

int     app_cache_init (void);
void    app_cache_exit (void);
void    app_cache_destroy (struct app_cache *cache);
struct app_cache *app_cache_create (uint16_t ox_mem_id, uint32_t cache_sz,
                        uint32_t ent_sz, uint16_t instances,
                        app_cache_rw_fn *read_fn, app_cache_rw_fn *write_fn);
struct app_cache_entry *app_cache_get (struct app_cache *cache, uint64_t lba);

/* ------- APPNVM CORE FUNCTIONS ------- */

struct app_global *oxapp (void);
int                app_mod_register(uint8_t modtype, uint8_t id, void *mod);
int                app_mod_set (uint8_t *modset);

/* ------- OLD MODULES REGISTRATION --------- */

void nr_blk_md_register (void);
void nr_ch_map_register (void);

/* ------- OX-BLOCK MODULES REGISTRATION ------- */

void app_channels_register (void);

void oxb_bbt_byte_register (void);
void oxb_blk_md_register (void);
void oxb_ch_prov_register (void);
void oxb_gl_prov_register (void);
void oxb_ch_map_register (void);
void oxb_gl_map_register (void);
void oxb_ppa_io_register (void);
void oxb_lba_io_register (void);
void oxb_gc_register (void);
void oxb_log_register (void);
void oxb_recovery_register ();

/* ------- OTHER FUNCTIONS ------- */

uint64_t get_log_tail (void);
void     set_log_head (uint64_t new_head);
uint32_t get_gl_prov_current_ch (void);

#endif /* APP_H */
