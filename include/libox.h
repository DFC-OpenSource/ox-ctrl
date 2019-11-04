#ifndef LIBOX_H
#define LIBOX_H

#include <unistd.h>
#include <stdint.h>
#include <sys/time.h>
#include <sys/queue.h>
#include <syslog.h>
#include <pthread.h>
#include <ox-lightnvm.h>
#include <ox-uatomic.h>

typedef struct NvmeCtrl      NvmeCtrl;
typedef struct NvmeCmd       NvmeCmd;
typedef struct NvmeCqe       NvmeCqe;
typedef struct NvmeSGLDesc   NvmeSGLDesc;
typedef struct NvmeNamespace NvmeNamespace;
typedef struct NvmeRequest   NvmeRequest;

#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)

#define VERSION     CONFIG_VERSION
#define PATCH       CONFIG_PATCHLEVEL
#define SUBLEVEL    CONFIG_SUBLEVEL
#define LABEL       CONFIG_LABEL
#define OX_VER      STR(VERSION) "." STR(PATCH) "." STR(SUBLEVEL)
#define OX_LABEL    OX_VER "-" LABEL

#define log_err(format, ...)         syslog(LOG_ERR, format, ## __VA_ARGS__)
#define log_info(format, ...)        syslog(LOG_INFO, format, ## __VA_ARGS__)

#define GET_NANOSECONDS(ns,ts) do {                                     \
        clock_gettime(CLOCK_REALTIME,&ts);                              \
        (ns) = ((ts).tv_sec * 1000000000 + (ts).tv_nsec);               \
} while ( 0 )

#define GET_MICROSECONDS(us,ts) do {                                    \
        clock_gettime(CLOCK_REALTIME,&ts);                              \
        (us) = ( ((ts).tv_sec * 1000000) + ((ts).tv_nsec / 1000) );     \
} while ( 0 )

#define TV_ELAPSED_USEC(tvs,tve,usec) do {                              \
        (usec) = ((tve).tv_sec*(uint64_t)1000000+(tve).tv_usec) -       \
        ((tvs).tv_sec*(uint64_t)1000000+(tvs).tv_usec);                 \
} while ( 0 )

#define TS_ADD_USEC(ts,tv,usec) do {                                    \
        (ts).tv_sec = (tv).tv_sec;                                      \
        gettimeofday (&tv, NULL);                                       \
        (ts).tv_sec += ((tv).tv_usec + (usec) >= 1000000) ? 1 : 0;      \
        (ts).tv_nsec = ((tv).tv_usec + (usec) >= 1000000) ?             \
                        ((usec) - (1000000 - (tv).tv_usec)) * 1000 :    \
                        ((tv).tv_usec + (usec)) * 1000;                 \
} while ( 0 )

#undef	MAX
#define MAX(a, b)  (((a) > (b)) ? (a) : (b))

#undef	MIN
#define MIN(a, b)  (((a) < (b)) ? (a) : (b))

#define AND64                  0xffffffffffffffff
#define ZERO_32FLAG            0x00000000
#define SEC64                  (1000000 & AND64)

#define NVM_CH_IN_USE          0x3c

#define MAX_NAME_SIZE           31
#define NVM_FTL_QUEUE_SIZE      2048

/* Timeout 2 sec */
#define NVM_QUEUE_RETRY         10000
#define NVM_QUEUE_RETRY_SLEEP   200

/* Timeout 10 sec */
#define NVM_FTL_QUEUE_TO        10 * 1000000

#define NVM_SYNCIO_TO          10
#define NVM_SYNCIO_FLAG_BUF    0x1
#define NVM_SYNCIO_FLAG_SYNC   0x2
#define NVM_SYNCIO_FLAG_DEC    0x4
#define NVM_SYNCIO_FLAG_MPL    0x8

#define NVM_FULL_UPDOWN        0x1
#define NVM_RESTART            0x0

#define NVM_TRANSP_PCI         0x1
#define NVM_TRANSP_FABRICS     0x2

/* Raw flash translation layers built directly on the OX core */
#define FTL_ID_LNVM            0x1
#define FTL_ID_OXAPP           0x10
/* Flash translation layers built with OX-App */
#define FTL_ID_BLOCK           0x1
#define FTL_ID_ELEOS           0x2

/* Set this macro to enable ox memory management component  */
#define OX_MEM_MANAGER          1

/* Set this macro to enable thread affinity in OX queues */
#define OX_TH_AFFINITY		0

enum {
    NVM_DMA_TO_HOST        = 0x0,
    NVM_DMA_FROM_HOST      = 0x1,
    NVM_DMA_SYNC_READ      = 0x2,
    NVM_DMA_SYNC_WRITE     = 0x3
};

enum {
    MMGR_READ_PG   = 0x1,
    MMGR_READ_OOB  = 0x2,
    MMGR_WRITE_PG  = 0x3,
    MMGR_BAD_BLK   = 0x5,
    MMGR_ERASE_BLK = 0x7,
    MMGR_READ_SGL  = 0x8,
    MMGR_WRITE_SGL = 0x9,
    MMGR_WRITE_PL_PG = 0x10,
    MMGR_READ_PL_PG = 0x11,
};

enum NVM_ERROR {
    EMAX_NAME_SIZE   = 0x1,
    EMMGR_REGISTER   = 0x2,
    EPCIE_REGISTER   = 0x3,
    EFTL_REGISTER    = 0x4,
    ENVME_REGISTER   = 0x5,
    ECH_CONFIG       = 0x6,
    EMEM             = 0x7,
    ENOMMGR          = 0x8,
    ENOFTL           = 0x9,
    EPARSER_REGISTER = 0xa,
    ENOPARSER        = 0xb,
    ENOTRANSP        = 0xc,
    ETRANSP_REGISTER = 0xd,
};

enum {
    NVM_IO_SUCCESS     = 0x1,
    NVM_IO_FAIL        = 0x2,
    NVM_IO_PROCESS     = 0x3,
    NVM_IO_NEW         = 0x4,
    NVM_IO_TIMEOUT     = 0x5
};

enum RUN_FLAGS {
    RUN_READY      = (1 << 0),
    RUN_NVME_ALLOC = (1 << 1),
    RUN_MMGR       = (1 << 2),
    RUN_FTL        = (1 << 3),
    RUN_CH         = (1 << 4),
    RUN_TRANSPORT  = (1 << 5),
    RUN_NVME       = (1 << 6),
    RUN_OXAPP      = (1 << 7),
    RUN_FABRICS    = (1 << 8),
    RUN_PARSER     = (1 << 9)
};

enum OX_MEM_TYPES {
    OX_MEM_CMD_ARG      = 0,
    OX_MEM_ADMIN        = 1,
    OX_MEM_CORE_INIT    = 2,
    OX_MEM_CORE_EXEC    = 3,
    OX_MEM_OX_MQ        = 4,
    OX_MEM_FTL_LNVM     = 5,
    OX_MEM_MMGR         = 6,
    OX_MEM_TCP_SERVER   = 7,
    OX_MEM_MMGR_VOLT    = 8,
    OX_MEM_MMGR_OCSSD   = 9,
    OX_MEM_FTL          = 10,
    OX_MEM_OXAPP        = 11,
    OX_MEM_APP_TRANS    = 12,
    OX_MEM_APP_CH       = 13,
    OX_MEM_OXBLK_LOG    = 14,
    OX_MEM_OXBLK_LBA    = 15,
    OX_MEM_OXBLK_GPR    = 16,
    OX_MEM_OXBLK_GMAP   = 17,
    OX_MEM_OXBLK_GC     = 18,
    OX_MEM_OXBLK_CPR    = 19,
    OX_MEM_OXBLK_REC    = 20,
    OX_MEM_FABRICS      = 21,
    OX_MEM_NVMEF        = 22,
    OX_MEM_ELEOS_W      = 29,
    OX_MEM_ELEOS_LBA    = 30,
    OX_MEM_APP_HMAP     = 31 /* 31-40 belong to HMAP instances */
};

enum ox_stats_recovery_types {
    /* Times in microseconds */
    OX_STATS_REC_CP_READ_US = 0,
    OX_STATS_REC_BBT_US,
    OX_STATS_REC_BLK_US,
    OX_STATS_REC_CH_MAP_US,
    OX_STATS_REC_GL_MAP_US,
    OX_STATS_REC_REPLAY_US,
    OX_STATS_REC_CP_WRITE_US,
    OX_STATS_REC_START1_US,
    OX_STATS_REC_START2_US,

    /* Log replay information */
    OX_STATS_REC_LOG_PGS,
    OX_STATS_REC_LOG_SZ,
    OX_STATS_REC_DROPPED_LOGS,
    OX_STATS_REC_TR_COMMIT,
    OX_STATS_REC_TR_ABORT /* 14 */
};

enum ox_stats_cp_types {
    OX_STATS_CP_LOAD_ADDR = 0,
    OX_STATS_CP_MAP_EVICT,
    OX_STATS_CP_MAPMD_EVICT,
    OX_STATS_CP_BLK_EVICT,
    OX_STATS_CP_MAP_BIG_SZ,
    OX_STATS_CP_MAP_SMALL_SZ,
    OX_STATS_CP_MAP_TINY_SZ,
    OX_STATS_CP_BLK_SMALL_SZ,
    OX_STATS_CP_BLK_TINY_SZ,
    OX_STATS_CP_SZ           /* 9 */
};

struct nvm_ppa_addr {
    /* Generic structure for all addresses */
    union {
        struct {
            uint64_t sec   : 3;
            uint64_t pl    : 2;
            uint64_t ch    : 12;
            uint64_t lun   : 6;
            uint64_t pg    : 12;
            uint64_t blk   : 15;
            uint64_t rsv   : 14;
        } g;

        uint64_t ppa;
    };
};

struct nvm_memory_region {
    uint64_t     addr;
    uint64_t     paddr;
    uint64_t     size;
    uint8_t      is_valid;
};

typedef void (nvm_callback_fn) (void *arg);

struct nvm_callback {
    nvm_callback_fn *cb_fn;
    void            *opaque;
    uint64_t         ts;
};

struct nvm_io_status {
    uint8_t     status;       /* global status for the cmd */
    uint16_t    nvme_status;  /* status to send to host */
    uint32_t    pg_errors;    /* n of errors */
    uint32_t    total_pgs;
    uint16_t    pgs_p;        /* pages processed within iteration */
    uint16_t    pgs_s;        /* pages success in request */
    uint16_t    ret_t;        /* retried times */
    uint8_t     pg_map[8];    /* pgs to retry */
};

struct nvm_mmgr_io_cmd {
    struct nvm_io_cmd       *nvm_io;
    struct nvm_ppa_addr     ppa;
    struct nvm_channel      *ch;
    struct nvm_callback     callback;
    uint64_t                prp[32]; /* max of 32 sectors */
    uint64_t                md_prp;
    uint8_t                 status;
    uint8_t                 cmdtype;
    uint32_t                pg_index; /* pg index inside nvm_io_cmd */
    uint32_t                pg_sz;
    uint16_t                n_sectors;
    uint32_t                sec_sz;
    uint32_t                md_sz;
    uint16_t                sec_offset; /* first sector in the ppa vector */
    uint8_t                 force_sync_md;
    uint8_t                 force_sync_data[32];
    u_atomic_t              *sync_count;
    pthread_mutex_t         *sync_mutex;
    struct timeval          tstart;
    struct timeval          tend;

    /* MMGR specific */
    //uint8_t                 rsvd[170];    /* Obsolete FPGA */
    uint8_t                 rsvd[128];       /* Volt + ELEOS */
};

struct nvm_io_cmd {
    uint64_t                    cid;
    struct nvm_channel          *channel[64];
    struct nvm_ppa_addr         ppalist[256];
    struct nvm_io_status        status;
    struct nvm_mmgr_io_cmd      mmgr_io[64];
    struct nvm_callback         callback;
    void                        *req;
    void                        *mq_req;
    void                        *opaque;
    uint64_t                    prp[256]; /* maximum 1 MB for block I/O */
    uint64_t                    md_prp[256];
    uint32_t                    sec_sz;
    uint32_t                    md_sz;
    uint32_t                    n_sec;
    uint64_t                    slba;
    uint8_t                     cmdtype;
    pthread_mutex_t             mutex;
};

#include <nvme.h>

struct nvm_mmgr;
typedef int     (nvm_mmgr_read_pg)(struct nvm_mmgr_io_cmd *);
typedef int     (nvm_mmgr_write_pg)(struct nvm_mmgr_io_cmd *);
typedef int     (nvm_mmgr_erase_blk)(struct nvm_mmgr_io_cmd *);
typedef int     (nvm_mmgr_get_ch_info)(struct nvm_channel *, uint16_t);
typedef int     (nvm_mmgr_set_ch_info)(struct nvm_channel *, uint16_t);
typedef void    (nvm_mmgr_exit)(struct nvm_mmgr *);

struct nvm_mmgr_ops {
    nvm_mmgr_read_pg       *read_pg;
    nvm_mmgr_write_pg      *write_pg;
    nvm_mmgr_erase_blk     *erase_blk;
    nvm_mmgr_exit          *exit;
    nvm_mmgr_get_ch_info   *get_ch_info;
    nvm_mmgr_set_ch_info   *set_ch_info;
};

struct nvm_mmgr_geometry {
    uint8_t     n_of_ch;
    uint8_t     lun_per_ch;
    uint16_t    blk_per_lun;
    uint16_t    pg_per_blk;
    uint16_t    sec_per_pg;
    uint8_t     n_of_planes;
    uint32_t    pg_size;
    uint32_t    sec_oob_sz;

    /* calculated values */
    uint32_t    sec_per_pl_pg;
    uint32_t    sec_per_blk;
    uint32_t    sec_per_lun;
    uint32_t    sec_per_ch;
    uint32_t    pg_per_lun;
    uint32_t    pg_per_ch;
    uint32_t    blk_per_ch;
    uint64_t    tot_sec;
    uint64_t    tot_pg;
    uint32_t    tot_blk;
    uint32_t    tot_lun;
    uint32_t    sec_size;
    uint32_t    pl_pg_size;
    uint32_t    blk_size;
    uint64_t    lun_size;
    uint64_t    ch_size;
    uint64_t    tot_size;
    uint32_t    pg_oob_sz;
    uint32_t    pl_pg_oob_sz;
    uint32_t    blk_oob_sz;
    uint32_t    lun_oob_sz;
    uint64_t    ch_oob_sz;
    uint64_t    tot_oob_sz;
};

enum mmgr_flags {
    MMGR_FLAG_PL_CMD        = (1 << 0), /* Accept multi plane page commands */
    MMGR_FLAG_MIN_CP_TIME   = (1 << 2)  /* Use the minimum checkpoint interval*/
};

struct nvm_mmgr {
    const char                  *name;
    struct nvm_mmgr_ops         *ops;
    struct nvm_mmgr_geometry    *geometry;
    struct nvm_channel          *ch_info;
    uint8_t                     flags;
    LIST_ENTRY(nvm_mmgr)        entry;
};

struct nvm_ftl_cap_get_bbtbl_st {
    struct nvm_ppa_addr ppa;
    uint8_t             *bbtbl;
    uint32_t            nblk;
    uint16_t            bb_format;
};

struct nvm_ftl_cap_set_bbtbl_st {
    struct nvm_ppa_addr ppa;
    uint8_t             value;
    uint16_t            bb_format;
};

struct nvm_ftl_cap_gl_fn {
    uint16_t            ftl_id;
    uint16_t            fn_id;
    void                *arg;
};

/* --- FTL CAPABILITIES BIT OFFSET --- */

enum {
    /* Get/Set Bad Block Table support */
    FTL_CAP_GET_BBTBL           = 0x00,
    FTL_CAP_SET_BBTBL           = 0x01,
    /* Get/Set Logical to Physical Table support */
    FTL_CAP_GET_L2PTBL          = 0x02,
    FTL_CAP_SET_L2PTBL          = 0x03,
    /* Application function support */
    FTL_CAP_INIT_FN             = 0x04,
    FTL_CAP_EXIT_FN             = 0x05,
    FTL_CAP_CALL_FN             = 0X06
};

/* --- FTL BAD BLOCK TABLE FORMATS --- */

enum {
   /* Each block within a LUN is represented by a byte, the function must return
   an array of n bytes, where n is the number of blocks per LUN. The function
   must set single bad blocks or accept an array of blocks to set as bad. */
    FTL_BBTBL_BYTE     = 0x00,
};

struct nvm_ftl;
typedef int       (nvm_ftl_submit_io)(struct nvm_io_cmd *);
typedef void      (nvm_ftl_callback_io)(struct nvm_mmgr_io_cmd *);
typedef int       (nvm_ftl_init_channel)(struct nvm_channel *);
typedef void      (nvm_ftl_exit)(void);
typedef int       (nvm_ftl_get_bbtbl)(struct nvm_ppa_addr *,uint8_t *,uint32_t);
typedef int       (nvm_ftl_set_bbtbl)(struct nvm_ppa_addr *, uint8_t);
typedef int       (nvm_ftl_init_fn)(uint16_t, void *arg);
typedef void      (nvm_ftl_exit_fn)(uint16_t);
typedef int       (nvm_ftl_call_fn)(uint16_t, void *arg);

struct nvm_ftl_ops {
    nvm_ftl_submit_io      *submit_io; /* FTL queue request consumer */
    nvm_ftl_callback_io    *callback_io;
    nvm_ftl_init_channel   *init_ch;
    nvm_ftl_exit           *exit;
    nvm_ftl_get_bbtbl      *get_bbtbl;
    nvm_ftl_set_bbtbl      *set_bbtbl;
    nvm_ftl_init_fn        *init_fn;
    nvm_ftl_exit_fn        *exit_fn;
    nvm_ftl_call_fn        *call_fn;
};

struct nvm_ftl {
    uint16_t                ftl_id;
    const char              *name;
    struct nvm_ftl_ops      *ops;
    uint32_t                cap; /* Capability bits */
    uint16_t                bbtbl_format;
    uint8_t                 nq; /* Number of queues/threads, up to 64 per FTL */
    struct ox_mq            *mq;
    uint16_t                next_queue[2];
    pthread_spinlock_t      next_queue_spin[2];
    LIST_ENTRY(nvm_ftl)     entry;
};

typedef void        (nvm_pcie_isr_notify)(void *);
typedef void        (nvm_pcie_exit)(void);
typedef void       *(nvm_pcie_nvme_consumer) (void *);
typedef void        (nvm_pcie_reset) (void);
typedef int         (nvm_pcie_dma) (void *buf, uint32_t size, uint64_t prp,
                                                                  uint8_t dir);

struct nvm_pcie_ops {
    nvm_pcie_nvme_consumer  *nvme_consumer;
    nvm_pcie_isr_notify     *isr_notify; /* notify host about completion */
    nvm_pcie_exit           *exit;
    nvm_pcie_reset          *reset;
    nvm_pcie_dma            *dma;
};

struct nvm_pcie {
    const char                  *name;
    void                        *ctrl;            /* pci specific structure */
    union NvmeRegs              *nvme_regs;
    struct nvm_pcie_ops         *ops;
    struct nvm_memory_region    *host_io_mem;     /* host BAR */
    pthread_t                   io_thread;        /* single thread for now */
    uint32_t                    *io_dbstride_ptr; /* for queue scheduling */
    uint8_t                     running;
};

typedef void (nvm_fabrics_exit) (void);
typedef int  (nvm_fabrics_create_queue) (uint16_t qid);
typedef void (nvm_fabrics_destroy_queue) (uint16_t qid);
typedef int  (nvm_fabrics_complete) (NvmeCqe *cqe, void *ctx);
typedef int  (nvm_fabrics_rdma) (void *buf, uint32_t size, uint64_t prp,
                                                                  uint8_t dir);

struct nvm_fabrics_ops {
    nvm_fabrics_create_queue    *create;
    nvm_fabrics_destroy_queue   *destroy;
    nvm_fabrics_complete        *complete;
    nvm_fabrics_rdma            *rdma;
    nvm_fabrics_exit            *exit;
};

struct nvm_fabrics {
    const char              *name;
    struct nvm_fabrics_ops  *ops;
};

#define NVM_MAGIC           0x3c
#define NVM_TRANS_TO_NVM    0
#define NVM_TRANS_FROM_NVM  1
#define NVM_IO_NORMAL       0
#define NVM_IO_RESERVED     1 /* Used for FTL reserved blocks */

struct nvm_magic {
    uint32_t rsvd;
    uint32_t magic;
} __attribute__((packed));

struct nvm_io_data {
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

struct nvm_channel {
    uint16_t                    ch_id;
    uint16_t                    ch_mmgr_id;
    uint64_t                    ns_pgs;
    uint64_t                    slba;
    uint64_t                    elba;
    uint64_t                    tot_bytes;
    uint16_t                    mmgr_rsv; /* number of blks reserved by mmgr */
    uint16_t                    ftl_rsv;  /* number of blks reserved by ftl */
    struct nvm_mmgr             *mmgr;
    struct nvm_ftl              *ftl;
    struct nvm_mmgr_geometry    *geometry;
    struct nvm_ppa_addr         *mmgr_rsv_list; /* list of mmgr reserved blks */
    struct nvm_ppa_addr         *ftl_rsv_list;
    LIST_ENTRY(nvm_channel)     entry;
    union {
        struct {
            uint64_t   ns_id         :16;
            uint64_t   ns_part       :32;
            uint64_t   ftl_id        :8;
            uint64_t   in_use        :8;
        } i;
        uint64_t       nvm_info;
    };
};

#define NVM_CMD_ADMIN   0x0
#define NVM_CMD_IO      0x1

typedef int (ox_nvm_parser_fn)(NvmeRequest *req, NvmeCmd *cmd);

struct nvm_parser_cmd {
    const char           name[MAX_NAME_SIZE];
    uint8_t              opcode;
    uint8_t              queue_type;
    ox_nvm_parser_fn    *opcode_fn;
};

struct nvm_parser;
typedef void (ox_nvm_parser_exit) (struct nvm_parser *set);

struct nvm_parser {
    const char                 *name;
    ox_nvm_parser_exit         *exit;
    struct nvm_parser_cmd      *cmd;
    uint32_t                    cmd_count;
    LIST_ENTRY(nvm_parser)  entry;
};

#define CMDARG_LEN          32
#define CMDARG_FLAG_A       (1 << 0)
#define CMDARG_FLAG_S       (1 << 1)
#define CMDARG_FLAG_T       (1 << 2)
#define CMDARG_FLAG_L       (1 << 3)

#define OX_RUN_MODE         0x0
#define OX_ADMIN_MODE       0x1

struct nvm_init_arg
{
    /* GLOBAL */
    int         cmdtype;
    int         arg_num;
    uint32_t    arg_flag;
    /* CMD ADMIN */
    char        admin_task[CMDARG_LEN];
};

enum cmdtypes {
    CMDARG_START = 1,
    CMDARG_DEBUG,
    CMDARG_ADMIN,
    CMDARG_RESET
};

struct nvm_net_iface {
    char        addr[16];
    uint16_t    port;
};

struct core_struct {
    uint16_t                parser_count;
    uint16_t                mmgr_count;
    uint16_t                ftl_count;
    uint16_t                ftl_q_count;
    uint16_t                nvm_ch_count;
    uint16_t                net_ifaces_count;
    uint64_t                nvm_ns_size;
    uint32_t                run_flag;
    uint8_t                 debug;
    uint8_t                 nostart;
    uint16_t                std_transport;
    uint16_t                std_ftl;
    uint16_t                std_oxapp;
    uint8_t                 reset;
    uint8_t                 null;
    struct nvm_net_iface    net_ifaces[64];
    struct nvm_pcie         *nvm_pcie;
    struct nvm_fabrics      *nvm_fabrics;
    struct nvm_channel      **nvm_ch;
    struct NvmeCtrl         *nvme_ctrl;
    struct nvm_init_arg     *args_global;
    struct nvm_parser_cmd    parser_io[256]; /* NVMe supports  256 opcodes */
    struct nvm_parser_cmd    parser_admin[256];
};

typedef int (ox_module_init_fn) (void);

/* core functions */
int  ox_ctrl_start      (int argc, char **argv);
void ox_ctrl_clear      (void);
int  ox_add_mmgr        (ox_module_init_fn *fn);
int  ox_add_ftl         (ox_module_init_fn *fn);
int  ox_add_parser      (ox_module_init_fn *fn);
int  ox_add_transport   (ox_module_init_fn *fn);
int  ox_add_net_interface (const char *addr, uint16_t port);
int  ox_register_parser (struct nvm_parser *parser);
int  ox_register_mmgr   (struct nvm_mmgr *mmgr);
int  ox_register_ftl    (struct nvm_ftl *ftl);
int  ox_register_pcie_handler (struct nvm_pcie *pcie);
int  ox_register_fabrics (struct nvm_fabrics *fabrics);
void ox_execute_opcode  (uint16_t qid, NvmeRequest *req);
void ox_set_std_ftl     (uint8_t ftl_id);
void ox_set_std_oxapp   (uint8_t ftl_id);
void ox_set_std_transport (uint8_t transp_id);
int  ox_dma (void *ptr, uint64_t prp, ssize_t size, uint8_t direction);
void ox_mmgr_callback   (struct nvm_mmgr_io_cmd *cmd);
void ox_ftl_callback    (struct nvm_io_cmd *cmd);
int  ox_ftl_cap_exec    (uint8_t cap, void *arg);
int  ox_submit_mmgr     (struct nvm_mmgr_io_cmd *cmd);
int  ox_submit_ftl      (struct nvm_io_cmd *cmd);
int  ox_submit_sync_io  (struct nvm_channel *, struct nvm_mmgr_io_cmd *,
                                                            void *, uint8_t);
int  ox_contains_ppa    (struct nvm_ppa_addr *list, uint32_t list_sz,
                                                    struct nvm_ppa_addr ppa);
struct nvm_ftl  *ox_get_ftl_instance (uint16_t ftl_id);
struct nvm_mmgr *ox_get_mmgr_instance (void);
int    ox_restart (void);

/* media manager common functions */
int  mmgr_set_ch_info (struct nvm_channel *ch, uint16_t nc);
int  mmgr_get_ch_info (struct nvm_mmgr *mmgr, struct nvm_channel *ch,
                                               uint16_t nc, uint16_t rsv_blks);

/* FTL common functions */
struct  nvm_io_data *ftl_alloc_pg_io (struct nvm_channel *ch);
void    ftl_pg_io_prepare (struct nvm_channel *ch, struct nvm_io_data *data);
void    ftl_pl_pg_io_prepare (struct nvm_channel *ch,struct nvm_io_data *data);
void    ftl_free_pg_io (struct nvm_io_data *data);
int     ftl_io_rsv_blk (struct nvm_channel *ch, uint8_t cmdtype,
                                     void **buf_vec, uint16_t blk, uint16_t pg);
int     ftl_io_rsv_pl_blk (struct nvm_channel *ch, uint8_t cmdtype,
                                        void *buf, uint16_t blk, uint16_t pg);
int     ftl_pg_io (struct nvm_channel *ch, uint8_t cmdtype,
                                      void **buf_vec, struct nvm_ppa_addr *ppa);
int     ftl_pl_pg_io (struct nvm_channel *ch, uint8_t cmdtype,
                                      void *buf, struct nvm_ppa_addr *ppa);
int     ftl_blk_current_page (struct nvm_channel *lch,
                      struct nvm_io_data *io, uint16_t blk_id, uint16_t offset);
int     ftl_nvm_seq_transfer (struct nvm_io_data *io, struct nvm_ppa_addr *ppa,
        uint8_t *user_buf, uint16_t pgs, uint16_t ent_per_pg, uint32_t ent_left,
        size_t entry_sz, uint8_t direction, uint8_t reserved);
int     ftl_pg_io_switch (struct nvm_channel *ch, uint8_t cmdtype,
                        void **pl_vec, struct nvm_ppa_addr *ppa, uint8_t type);

/* NVMe controller functions */
int  nvme_init  (NvmeCtrl *n);
void nvme_exit  (void);
int  nvmef_init (NvmeCtrl *n);
void nvmef_exit (void);
int  nvmef_process_capsule  (NvmeCmd *cmd, void *ctx);
int  nvmef_create_queue     (uint16_t qid);
void nvmef_destroy_queue    (uint16_t qid);
void nvmef_complete_request (NvmeRequest *req);
int  nvmef_sgl_to_prp (uint32_t nlb, NvmeSGLDesc *desc, uint64_t *prp_buf,
                                                                uint64_t *keys);

/* NVMe transport funcions */
int  pcie_fpga_init  (void);
int  ox_fabrics_init (void);

/* admin mode functions */
int  ox_admin_init (struct nvm_init_arg *);

/* console functions */
int  ox_cmdarg_init (int, char **);
void ox_cmdarg_exit (void);
void ox_cmdline_init (void);

/* Memory management functions */
int          ox_mem_init (void);
void         ox_mem_exit (void);
uint64_t     ox_mem_total (void);
uint64_t     ox_mem_size (uint16_t type);

/* Statistics functions */
int  ox_stats_init (void);
void ox_stats_exit (void);
void ox_stats_add_io (struct nvm_mmgr_io_cmd *cmd, uint8_t synch, uint16_t tid);
void ox_stats_add_gc (uint16_t type, uint8_t is_write, uint32_t count);
void ox_stats_add_event (uint16_t type, uint32_t count);
void ox_stats_add_rec (uint16_t type, uint64_t value);
void ox_stats_add_log (uint16_t type, uint64_t count);
void ox_stats_add_cp (uint16_t type, uint64_t value);
void ox_stats_set_cp (uint16_t type, uint64_t value);
void ox_stats_reset_io (void);
void ox_stats_print_io (void);
void ox_stats_print_gc (void);
void ox_stats_print_recovery (void);
void ox_stats_print_checkpoint (void);

#if OX_MEM_MANAGER
void        *ox_malloc  (size_t size, uint16_t type);
void        *ox_calloc  (size_t members, size_t size, uint16_t type);
void        *ox_realloc (void *ptr, size_t size, uint16_t type);
void        *ox_free    (void *ptr, uint16_t type);
#else
static inline void *ox_malloc (size_t size, uint16_t type) {
    return malloc (size);
}
static inline void *ox_calloc (size_t members, size_t size, uint16_t type) {
    return calloc (members, size);
}
static inline void *ox_realloc (void *ptr, size_t size, uint16_t type) {
    return realloc (ptr, size);
}
static inline void *ox_free (void *ptr, uint16_t type) {
    free (ptr);
    return NULL;
}
#endif

void         ox_mem_print_memory (void);
struct ox_mem_type *ox_mem_create_type (const char *name, uint16_t type);

/* Module registration */
int ftl_lnvm_init (void);
int ftl_oxapp_init (void);
int mmgr_dfcnand_init (void);
int mmgr_ocssd_1_2_init (void);
int mmgr_ocssd_2_0_init (void);
int mmgr_volt_init (void);
int mmgr_volt_init_nodisk (void);
int parser_nvme_init (void);
int parser_lnvm_init (void);
int parser_fabrics_init (void);
int parser_eleos_init (void);

#endif	// LIBOX_H
