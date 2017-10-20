#define _GNU_SOURCE

#include <stdio.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <errno.h>
#include <sched.h>
#include <pthread.h>
#include <sys/stat.h>
#include <signal.h>
#include <assert.h>
#include "nand_dma.h"
#include <sys/time.h>

#include "../../include/ssd.h"
#include "fpga_3_01_00/nand_dma.h"

#define RESET_TIMEOUT 200000

static uint32_t desc_idx[TBL_COUNT];
static uint32_t desc_retry[TBL_COUNT];

Desc_Track Desc_trck[TBL_COUNT][DESC_PER_TBL];
Dma_Control DmaCtrl[TBL_COUNT];
csr_reg *csr[TBL_COUNT];
tblsz_reg *table_sz[TBL_COUNT];
uint8_t *table[TBL_COUNT];
uint8_t first_dma = 0;

int fd, CHIP_COUNT = 0;
uint8_t *desc_tbl_addr = NULL;
uint32_t *ctrl_reg_addr = NULL;
uint32_t *nand_gpio_int = NULL;
uint64_t bar_address[2][6];
int fpga_version = 0, size[2];

static pthread_mutex_t m_desc_mutex;
static uint8_t nand_running; /* stop completion thread */
static uint8_t reset_delay; /* delay threads during reset */
static uint8_t comp_reset; /* notify that completion thread is idle for reset */
pthread_t io_completion = 0;

struct ad_trans {
    unsigned long long int addr[70];
};

void affinity(int cpuid, pthread_t thread) {
    cpu_set_t cpuset;
    int ret_val;

    CPU_ZERO(&cpuset); /* Initializing the CPU set to be the empty set */
    CPU_SET(cpuid, &cpuset); /* setting CPU on cpuset */

    ret_val = pthread_setaffinity_np(thread, sizeof (cpu_set_t), &cpuset);
    if (ret_val != 0) {
        perror("pthread_setaffinity_np");
    }
    ret_val = pthread_getaffinity_np(thread, sizeof (cpu_set_t), &cpuset);
    if (ret_val != 0) {
        perror("pthread_getaffinity_np");
    }
}

int nand_page_prog(io_cmd *cmd_buf) {
    nand_cmd_struct *cmd_struct = &cmd_buf->nand_cmd;

    memset(cmd_struct, 0, sizeof (nand_cmd_struct));
    cmd_struct->row_addr = (cmd_buf->lun << 21) | ((cmd_buf->block & 0xFFF)
            << 9) | (cmd_buf->page & 0x1FF);
    cmd_struct->trgt_col_addr = cmd_buf->target << 29 | cmd_buf->col_addr;
    cmd_struct->db_LSB_0 = cmd_buf->host_addr[0] & 0xffffffff;
    cmd_struct->db_MSB_0 = cmd_buf->host_addr[0] >> 32;
    cmd_struct->db_LSB_1 = cmd_buf->host_addr[1] & 0xffffffff;
    cmd_struct->db_MSB_1 = cmd_buf->host_addr[1] >> 32;
    cmd_struct->db_LSB_2 = cmd_buf->host_addr[2] & 0xffffffff;
    cmd_struct->db_MSB_2 = cmd_buf->host_addr[2] >> 32;
    cmd_struct->db_LSB_3 = cmd_buf->host_addr[3] & 0xffffffff;
    cmd_struct->db_MSB_3 = cmd_buf->host_addr[3] >> 32;
    cmd_struct->len1_len0 = cmd_buf->len[1] << 16 | cmd_buf->len[0];
    cmd_struct->len3_len2 = cmd_buf->len[3] << 16 | cmd_buf->len[2];
    cmd_struct->oob_LSB = cmd_buf->host_addr[4] & 0xffffffff;
    cmd_struct->oob_len_MSB = cmd_buf->len[4] << 20 | ((cmd_buf->host_addr[4]
            >> 32) & 0xfffff);
    if ((fpga_version == 0x00030100) || (fpga_version == 0x00030004)) {
        cmd_struct->control_fields = PAGE_PROG << 5 |
                (!(cmd_buf->block % 2)) << 4 | cmd_buf->dfc_io.rdy_bsy | 0;
    } else if (fpga_version == 0x00020502) {
        if (!(cmd_buf->lun) && !(cmd_buf->target)) {
            cmd_struct->control_fields = PAGE_PROG << 5 |
                    (!(cmd_buf->block % 2)) << 4 | 4 << 1 | 0;
        } else {
            cmd_struct->control_fields = PAGE_PROG << 5 |
                    (!(cmd_buf->block % 2)) << 4 | 0;
        }

    }
    make_desc(cmd_struct, cmd_buf->chip, WRITE, cmd_buf);

    return SUCCESS;
}

int nand_page_read(io_cmd *cmd_buf) {
    nand_cmd_struct *cmd_struct = &cmd_buf->nand_cmd;
    memset(cmd_struct, 0, sizeof (nand_cmd_struct));

    cmd_struct->row_addr = (cmd_buf->lun << 21) | ((cmd_buf->block & 0xFFF)
            << 9) | (cmd_buf->page & 0x1FF);
    cmd_struct->trgt_col_addr = cmd_buf->target << 29 | cmd_buf->col_addr;
    cmd_struct->db_LSB_0 = cmd_buf->host_addr[0] & 0xffffffff;
    cmd_struct->db_MSB_0 = cmd_buf->host_addr[0] >> 32;
    cmd_struct->db_LSB_1 = cmd_buf->host_addr[1] & 0xffffffff;
    cmd_struct->db_MSB_1 = cmd_buf->host_addr[1] >> 32;
    cmd_struct->db_LSB_2 = cmd_buf->host_addr[2] & 0xffffffff;
    cmd_struct->db_MSB_2 = cmd_buf->host_addr[2] >> 32;
    cmd_struct->db_LSB_3 = cmd_buf->host_addr[3] & 0xffffffff;
    cmd_struct->db_MSB_3 = cmd_buf->host_addr[3] >> 32;
    cmd_struct->len1_len0 = cmd_buf->len[1] << 16 | cmd_buf->len[0];
    cmd_struct->len3_len2 = cmd_buf->len[3] << 16 | cmd_buf->len[2];
    cmd_struct->oob_LSB = cmd_buf->host_addr[4] & 0xffffffff;
    cmd_struct->oob_len_MSB = cmd_buf->len[4] << 20 |
            ((cmd_buf->host_addr[4] >> 32) & 0xfffff);
    cmd_struct->control_fields = PAGE_READ << 5 | cmd_buf->dfc_io.rdy_bsy | 1;

    make_desc(cmd_struct, cmd_buf->chip, READ_IO, cmd_buf);

    return SUCCESS;
}

int nand_block_erase(io_cmd *cmd_buf) {
    nand_cmd_struct *cmd_struct = &cmd_buf->nand_cmd;
    memset(cmd_struct, 0, sizeof (nand_cmd_struct));

    cmd_struct->row_addr = (cmd_buf->lun << 21) | ((cmd_buf->block & 0xFFF)
            << 9);
    cmd_struct->trgt_col_addr = cmd_buf->target << 29 | 0x0;
    cmd_struct->db_LSB_0 = 0;
    cmd_struct->db_MSB_0 = 0;
    cmd_struct->control_fields = BLOCK_ERASE << 5 | 1 << 3 | 1 << 2;
    make_desc(cmd_struct, cmd_buf->chip, ERASE, NULL);
    memset(cmd_struct, 0, sizeof (nand_cmd_struct));
    /*Read Status*/
    cmd_struct->row_addr = (cmd_buf->lun << 21) | ((cmd_buf->block & 0xFFF)
            << 9);
    cmd_struct->trgt_col_addr = cmd_buf->target << 29 | 0x0;
    cmd_struct->db_LSB_0 = 0;
    cmd_struct->db_MSB_0 = 0;
    cmd_struct->len1_len0 = 0x2;
    cmd_struct->control_fields = READ_STATUS << 5 |
            cmd_buf->dfc_io.rdy_bsy | 1 << 2 | 1;
    make_desc(cmd_struct, cmd_buf->chip, READ_STS, cmd_buf);

    return SUCCESS;
}

int nand_reset() {
    int i;
    nand_cmd_struct *cmd_struct =
            (nand_cmd_struct *) malloc(sizeof (nand_cmd_struct));
    memset(cmd_struct, 0, sizeof (nand_cmd_struct));

    for (i = 0; i < CHIP_COUNT; i++) {
        int trgt;
        for (trgt = 0; trgt < 2; trgt++) {
            cmd_struct->row_addr = 0x0000;
            cmd_struct->trgt_col_addr = trgt << 29 | 0x0000;
            cmd_struct->db_LSB_0 = 0x0;
            cmd_struct->db_MSB_0 = 0x0;
            cmd_struct->control_fields = RESET << 5 | 1 << 2;
            make_desc(cmd_struct, i, OTHERS, NULL);
            memset(cmd_struct, 0, sizeof (nand_cmd_struct));
        }
    }

    free(cmd_struct);
    return SUCCESS;
}

int nand_set_feature(uint16_t feat_addr, uint16_t length, void *feature_data) {
    static int k = 0;
    int chip = 0, trgt = 0;
    nand_cmd_struct *cmd_struct =
            (nand_cmd_struct *) malloc(sizeof (nand_cmd_struct));
    memset(virt_addr[1]+(k * 0x1000), 0, 0x1000);
    memcpy(virt_addr[1]+(k * 1000), feature_data, length);
    for (chip = 0; chip < CHIP_COUNT; chip++) {
        for (trgt = 0; trgt < NAND_TARGET_COUNT; trgt++) {
            memset(cmd_struct, 0, sizeof (nand_cmd_struct));
            cmd_struct->row_addr = feat_addr; /*feature_address*/
            cmd_struct->trgt_col_addr = trgt << 29 | 0x0000;
            cmd_struct->db_LSB_0 = (phy_addr[1]+(k * 1000)) & 0xFFFFFFFF;
            cmd_struct->db_MSB_0 = (phy_addr[1]+(k * 1000)) >> 32;
            cmd_struct->len1_len0 = length;
            cmd_struct->control_fields = SET_FEATURE << 5 | 1 << 3 | 1 << 2;
            make_desc(cmd_struct, chip, OTHERS, NULL);
        }
    }
    k++;
    free(cmd_struct);
    return SUCCESS;
}

static void setup_nand_desc_array() {
    int i = 0, j, k = 0;
    uint8_t *dma_table;

    for (j = 0; j < CHIP_COUNT; j++) {
        dma_table = table[j];
        for (i = 0; i < DESC_PER_TBL; i++) {
            DmaCtrl[j].DescSt[i].ptr = dma_table + (i * DESC_SIZE);
            DmaCtrl[j].DescSt[i].valid = 1;
            DmaCtrl[j].DescSt[i].page_phy_addr = phy_addr[0]+(k * 4096);
            DmaCtrl[j].DescSt[i].page_virt_addr = virt_addr[0] + (k * 4096);
            mutex_init(&DmaCtrl[j].DescSt[i].available, NULL);
            k++;
        }
    }
}

static inline void reset_dma_descriptors(void) {
    int iter, i;
    csr_reg csr_reset = {0, 0, 0, 1, 1, 0};

    for (iter = 0; iter < CHIP_COUNT; iter++) {
        memcpy((void *) csr[iter], (void *) &csr_reset, sizeof (csr_reg));

        for (i = 0; i < (DESC_PER_TBL); i++) {
            memset((nand_descriptor*) table[iter] + i, 0, sizeof (nand_descriptor));
        }
    }
}

static void reset_all() {
    int tbl, index;

    comp_reset = 1;
    reset_delay = 1;

    while (comp_reset && io_completion)
        usleep (1);

    setup_nand_desc_array();
    reset_dma_descriptors();

    for (tbl = 0; tbl < CHIP_COUNT; tbl++) {
        DmaCtrl[tbl].head_idx = 0;
        desc_idx[tbl] = 0;
        desc_retry[tbl] = 0;

        table_sz[tbl]->table_size = 0x10;

        csr[tbl]->reset = 1;
        csr[tbl]->reset = 0;

        for (index = 0; index < DESC_PER_TBL; index++)
            memset (&Desc_trck[tbl][index], 0x0, sizeof (Desc_Track));
    }

    first_dma = 0;

    usleep(1000);
    nand_reset();
    usleep(1000);

    reset_delay = 0;
    comp_reset = 0;
}

uint8_t make_desc(nand_cmd_struct *cmd_struct, uint8_t tid, uint8_t req_type,
        void *req_ptr) {
    uint8_t i, tbl = 0, fdma = 0;
    nand_descriptor desc;
    uint16_t head_idx;

    if (reset_delay && req_type != OTHERS)
        return -1;

    if (first_dma == 0) {
        first_dma++;
        fdma++;
        for (tbl = 0; tbl < CHIP_COUNT; tbl++) {
            csr[tbl]->reset = 1;
            csr[tbl]->reset = 0;
        }
    }

    desc_retry[tid] = 0;

    do {
        if (reset_delay && req_type != OTHERS)
            return -1;
        else if (desc_retry[tid] >= RESET_TIMEOUT)
            goto RESET;

        if (Desc_trck[tid][desc_idx[tid]].is_used == TRACK_IDX_FREE) {
            break;
        }
        desc_retry[tid]++;
        usleep(1);
    } while (1);

    desc_retry[tid] = 0;

    do {
        if (reset_delay && req_type != OTHERS)
            return -1;
        else if (desc_retry[tid] >= RESET_TIMEOUT)
            goto RESET;

        head_idx = DmaCtrl[tid].head_idx;
        mutex_lock(&DmaCtrl[tid].DescSt[head_idx].available);
        if (DmaCtrl[tid].DescSt[head_idx].valid) {
            break;
        }
        mutex_unlock(&DmaCtrl[tid].DescSt[head_idx].available);
        desc_retry[tid]++;
        usleep(1);
    } while (1);

    if (cmd_struct) {
        if (req_type == READ_STS) {
            cmd_struct->db_LSB_0 = DmaCtrl[tid].DescSt[head_idx].page_phy_addr
                    & 0xffffffff;
            cmd_struct->db_MSB_0 = DmaCtrl[tid].DescSt[head_idx].page_phy_addr
                    >> 32;
        }
    } else {
        DmaCtrl[tid].DescSt[head_idx].req_type = req_type;
        DmaCtrl[tid].DescSt[head_idx].valid = 0;

        Desc_trck[tid][desc_idx[tid]].DescSt_ptr =
                &(DmaCtrl[tid].DescSt[head_idx]);
        Desc_trck[tid][desc_idx[tid]].is_used = TRACK_IDX_USED;
        mutex_unlock(&DmaCtrl[tid].DescSt[head_idx].available);
        return 0;
    }
    memset(&desc, 0, sizeof (nand_descriptor));
    desc.row_addr = cmd_struct->row_addr;
    desc.column_addr = cmd_struct->trgt_col_addr & 0x1fffffff;
    desc.target = cmd_struct->trgt_col_addr >> 29;
    desc.data_buff0_LSB = cmd_struct->db_LSB_0;
    desc.data_buff0_MSB = cmd_struct->db_MSB_0;
    desc.data_buff1_LSB = cmd_struct->db_LSB_1;
    desc.data_buff1_MSB = cmd_struct->db_MSB_1;
    desc.data_buff2_LSB = cmd_struct->db_LSB_2;
    desc.data_buff2_MSB = cmd_struct->db_MSB_2;
    desc.data_buff3_LSB = cmd_struct->db_LSB_3;
    desc.data_buff3_MSB = cmd_struct->db_MSB_3;
    desc.length0 = cmd_struct->len1_len0 & 0xffff;
    desc.length1 = cmd_struct->len1_len0 >> 16;
    desc.length2 = cmd_struct->len3_len2 & 0xffff;
    desc.length3 = cmd_struct->len3_len2 >> 16;
    desc.oob_data_LSB = cmd_struct->oob_LSB;
    desc.oob_data_MSB = cmd_struct->oob_len_MSB & 0xfffff;
    desc.oob_length = cmd_struct->oob_len_MSB >> 20;
    desc.chn_buff_id = cmd_struct->buffer_channel_id;
    desc.dir = cmd_struct->control_fields & 1;
    desc.ecc = (cmd_struct->control_fields >> 2) & 1;
    desc.rdy_busy = (cmd_struct->control_fields >> 3) & 1;
    desc.chng_pln = (cmd_struct->control_fields >> 4) & 1;
    desc.command = (cmd_struct->control_fields >> 5) & 0x3f;
    desc.irq_en = 1;
    desc.hold = 0;
    desc.desc_id = head_idx + 1;
    desc.OwnedByfpga = 1;

    if (req_type != END_DESC) {
        memcpy(DmaCtrl[tid].DescSt[head_idx].ptr, &desc,
                sizeof (nand_descriptor));
    }

    DmaCtrl[tid].DescSt[head_idx].req_ptr = req_ptr;
    DmaCtrl[tid].DescSt[head_idx].req_type = req_type;
    DmaCtrl[tid].DescSt[head_idx].valid = 0;

    if (fdma) {
        for (tbl = 0; tbl < CHIP_COUNT; tbl++) {
            csr[tbl]->start = 1;
            csr[tbl]->loop = 1;
        }
    }
    DmaCtrl[tid].DescSt[head_idx].idx = head_idx;
    Desc_trck[tid][desc_idx[tid]].DescSt_ptr = &(DmaCtrl[tid].DescSt[head_idx]);
    Desc_trck[tid][desc_idx[tid]].is_used = TRACK_IDX_USED;

    mutex_unlock(&DmaCtrl[tid].DescSt[head_idx].available);

    CIRC_INCR(desc_idx[tid], DESC_PER_TBL);
    CIRC_INCR(DmaCtrl[tid].head_idx, DESC_PER_TBL);

    return 0;

RESET:
    if (req_type == OTHERS)
        return -1;

    log_info("[dfc_nand WARNING: Reseting channels...]\n");
    reset_all();

    return -1;
}

void *io_completer() {
    csf_reg *csf;
    csf_reg *csf_bak = malloc(sizeof (csf_reg));
    io_cmd *cmd;
    uint8_t io_st, tbl = 0;
    int err, ret = 0, i;
    uint32_t intr_enable = 1;
    uint64_t desc_tbl;
    uint64_t desc_idx[DESC_PER_TBL];
    uint32_t csf_val = 0;

    int seln, fd, configfd, count, nb;
    fd_set readfds;
    unsigned char command_high;
    static struct timeval def_time = {0, 100};
    static struct timeval timeout = {0, 100};

#ifdef INTR
    fd = open("/dev/uio1", O_RDWR);
    if (fd < 0) {
        perror("open uio1");
    }
    FD_ZERO(&readfds);
#endif

RESET:
    desc_tbl = 0;
    for (i = 0; i < TBL_COUNT; i++)
        desc_idx[i] = 0;
    comp_reset = 0;
    usleep (1);

    while (!nand_running) {
#ifdef INTR
        timeout = def_time;
WAIT_INT:
        FD_SET(fd, &readfds);
        nb = write(fd, &intr_enable, sizeof (intr_enable));
        if (nb < sizeof (intr_enable)) {
            perror("config write:");
            continue;
        }

        *(nand_gpio_int) = *(nand_gpio_int) & (~(0x4));

        seln = select(fd + 1, &readfds, NULL, NULL, &timeout);
        if (seln > 0 && FD_ISSET(fd, &readfds)) {
            read(fd, &count, sizeof (uint32_t));
        }
#endif

        while (!nand_running) {
            do {
NEXT_CH:
                if (reset_delay)
                    goto RESET;
                if (nand_running)
                    goto OUT;

                if (Desc_trck[desc_tbl][desc_idx[desc_tbl]].is_used ==
                                                              TRACK_IDX_USED) {
                    break;
                }
#ifdef INTR
                if (desc_tbl == TBL_COUNT - 1) {
                    desc_tbl = 0;
                    goto WAIT_INT;
                } else {
                    desc_tbl++;
                    goto NEXT_CH;
                }
#else
                usleep(1);
#endif
            } while (1);

            do {
                if (reset_delay)
                    goto RESET;
                if (nand_running)
                    goto OUT;

                mutex_lock(&(Desc_trck[desc_tbl][desc_idx[desc_tbl]].
                                                       DescSt_ptr->available));
                if (!Desc_trck[desc_tbl][desc_idx[desc_tbl]]
                                                          .DescSt_ptr->valid) {
                    break;
                }
                mutex_unlock(&(Desc_trck[desc_tbl][desc_idx[desc_tbl]].
                                                       DescSt_ptr->available));
#ifdef INTR
                if (desc_tbl == TBL_COUNT - 1) {
                    desc_tbl = 0;
                    goto WAIT_INT;
                } else {
                    desc_tbl++;
                    goto NEXT_CH;
                }
#else
                usleep(1);
#endif
            } while (1);

            if (Desc_trck[desc_tbl][desc_idx[desc_tbl]].DescSt_ptr->req_type
                    == END_DESC) {
                mutex_unlock(&(Desc_trck[desc_tbl][desc_idx[desc_tbl]].
                                                       DescSt_ptr->available));
                return 0;
            }

            do {
                if (reset_delay || nand_running)
                    mutex_unlock(&(Desc_trck[desc_tbl][desc_idx[desc_tbl]].
                                                       DescSt_ptr->available));
                if (reset_delay)
                    goto RESET;
                if (nand_running)
                    goto OUT;

                csf = (csf_reg *) ((uint32_t*) (Desc_trck[desc_tbl]
                        [desc_idx[desc_tbl]].DescSt_ptr->ptr) + 15);
                csf_val = *((uint32_t *) csf);
                csf = (csf_reg *) & csf_val;
                if (!csf->OwnedByFpga) {
                    break;
                }
#ifdef INTR
                mutex_unlock(&(Desc_trck[desc_tbl][desc_idx[desc_tbl]].
                        DescSt_ptr->available));
                if (desc_tbl == TBL_COUNT - 1) {
                    desc_tbl = 0;
                    goto WAIT_INT;
                } else {
                    desc_tbl++;
                    goto NEXT_CH;
                }
#else
                usleep(1);
#endif
            } while (1);

            memcpy(csf_bak, csf, sizeof (csf_reg));
            if (csf_bak->dma_cmp) {
                io_st = 0;
                if (Desc_trck[desc_tbl][desc_idx[desc_tbl]].
                        DescSt_ptr->req_ptr) {
                    cmd = Desc_trck[desc_tbl][desc_idx[desc_tbl]].
                            DescSt_ptr->req_ptr;
                    if (Desc_trck[desc_tbl][desc_idx[desc_tbl]].
                            DescSt_ptr->req_type == READ_STS) {
                        (*(Desc_trck[desc_tbl][desc_idx[desc_tbl]].
                                DescSt_ptr->page_virt_addr)
                                == 0xE0) ? (cmd->status = 1) : (cmd->status = 0);
                    } else {
                        cmd->status = 1;
                    }

                    dfcnand_callback(cmd);
                }
            } else {
                cmd = Desc_trck[desc_tbl][desc_idx[desc_tbl]].
                                                           DescSt_ptr->req_ptr;
                if (cmd) {;
                    err = csr[desc_tbl]->err_code;
                    log_info("[dfc_nand: FPGA IO error. Error code: %d]\n",err);

                    cmd->status = (err) ? 0 : 1;

                    dfcnand_callback(cmd);
                }
            }

            Desc_trck[desc_tbl][desc_idx[desc_tbl]].is_used = TRACK_IDX_FREE;
            Desc_trck[desc_tbl][desc_idx[desc_tbl]].DescSt_ptr->valid = 1;

            mutex_unlock(&Desc_trck[desc_tbl][desc_idx[desc_tbl]].
                                                        DescSt_ptr->available);

            CIRC_INCR(desc_idx[desc_tbl], DESC_PER_TBL);

            if (desc_tbl == TBL_COUNT - 1)
                desc_tbl = 0;
            else
                desc_tbl++;
        }
    }
OUT:
    close (fd);
    return NULL;
}

/*Have allocated 4MB * 70 units of memory in uio_pci_generic driver.
 *  *  This function reads those physical address.*/

uint8_t read_mem_addr() {
    struct ad_trans buf;

    uint8_t fd, ret;

    fd = open("/dev/uio_rc", O_RDWR);
    if (fd < 0) {
        printf("uio_rc not found. Do insmod uio_pci_generic driver\n");
        ;
        return FAILURE;
    }

    memset(&buf, 0, sizeof (uint64_t)*70);
    ret = read(fd, (struct ad_trans *) &buf, sizeof (unsigned long long int)*70);
    if (ret == -1) {
        perror("read");
        return FAILURE;
    }

    memcpy(phy_addr, buf.addr, sizeof (unsigned long long int) *70);
    close(fd);
    return SUCCESS;
}

/*Get the virtual address for each 4MB physical memory units(70 units),
 * first virtual memory virt_addr[0] is used by the library, other 69-4MB
 * units can be used by the userspace application.*/


uint8_t virt_map() {
    uint32_t i;

    fd = open("/dev/mem", O_RDWR);
    if (fd < 0) {
        perror("open:");
        return FAILURE;
    }
    for (i = 0; i < 70; i++) {
        virt_addr[i] = (uint8_t*) mmap(0, 1024 * getpagesize(),
                PROT_READ | PROT_WRITE, MAP_SHARED, fd, phy_addr[i]);
        if (virt_addr[i] == (uint8_t *) MAP_FAILED) {
            perror("oob_map");
            close(fd);
            return FAILURE;
        }
    }

    return SUCCESS;
}

uint8_t virt_unmap() {
    uint32_t i;
    for (i = 0; i < 70; i++) {
        munmap(virt_addr[i], 1024 * getpagesize());
    }

    close(fd);
    return SUCCESS;
}

void init_dma_mgr() {
    int i = 0;
    uint32_t total_desc = 0;
    /*Based on Fpga Image version, DMA Table Offsets has been modified*/
    if ((fpga_version == 0x00030100) || (fpga_version == 0x00030004)) {
        for (i = 0; i < CHIP_COUNT; i++) {
            csr[i] = (csr_reg *) (ctrl_reg_addr + (i * NAND_CSR_OFFSET) + 0x1000);
            table_sz[i] = (tblsz_reg *) (ctrl_reg_addr +
                    (i * NAND_CSR_OFFSET) + TBL_SZ_SHIFT + 0x1000);
            table[i] = desc_tbl_addr + (i * NAND_TBL_OFFSET) + 0x10000;
            nand_gpio_int = ctrl_reg_addr + 0x4001;
        }
    } else if (fpga_version == 0x00020502) {
        for (i = 0; i < CHIP_COUNT; i++) {
            csr[i] = (csr_reg *) (ctrl_reg_addr + (i * NAND_CSR_OFFSET));
            table_sz[i] = (tblsz_reg *) (ctrl_reg_addr +
                    (i * NAND_CSR_OFFSET) + TBL_SZ_SHIFT);
            table[i] = desc_tbl_addr + (i * NAND_TBL_OFFSET);
            nand_gpio_int = ctrl_reg_addr + 0x4001;
        }
    }

    reset_all();
}

int mmap_fpga_regs(int mem_fd, uint64_t bar2_addr, uint64_t bar4_addr) {
    int ret = 0;
    if ((fpga_version == 0x00030100) || (fpga_version == 0x00030004)) {
        size[0] = 32;
        size[1] = 32;
    } else if (fpga_version == 0x00020502) {
        size[0] = 4;
        size[1] = 8;
    }
    desc_tbl_addr = mmap(0, size[0] * getpagesize(), PROT_READ |
            PROT_WRITE, MAP_SHARED, mem_fd, bar2_addr);
    if (desc_tbl_addr == MAP_FAILED) {
        perror("mmap:");
        ret = errno;
    }


    ctrl_reg_addr = (uint32_t*) mmap(0, size[1] * getpagesize(),
            PROT_READ | PROT_WRITE, MAP_SHARED, mem_fd, bar4_addr);
    if (ctrl_reg_addr == MAP_FAILED) {
        perror("mmap:");
        ret = errno;
    }

    return SUCCESS;
}

/*Getting The BAR addresses from device's resource*/
uint8_t get_address(int ep, int bar_no) {
    int word_no = 3 * bar_no;
    int count = 0;
    FILE *file;

    if (!ep) {
        file = fopen("/sys/bus/pci/devices/0000:01:00.0/resource", "r");
    } else {
        file = fopen("/sys/bus/pci/devices/0001:01:00.0/resource", "r");
    }

    if (file != NULL) {
        char word[20];
        while (fgets(word, sizeof (word), file) != NULL) {
            if (count == word_no) {
                bar_address[ep][bar_no] = (uint64_t) strtoul(word, NULL, 16);
                break;
            } else count++;
        }
    } else {
        printf("Endpoint not found.Check FPGA configured properly\n");
        return FAILURE;
    }
    fclose(file);
    return SUCCESS;
}

/* Reading the version register. Based on the version BARs of
 * Corresponding PEXs will be read from PCI device's resource*/
uint8_t read_bar_address() {
    int i, ret = 0, MAX_BARS = 6;
    uint32_t *ver_reg;

    for (i = 0; i < MAX_BARS; i++) {
        ret = get_address(0, i);
        if (ret) {
            perror("Get_Bar_Addr:");
        }
    }
    ver_reg = (uint32_t*) mmap(0, getpagesize(), PROT_READ |
            PROT_WRITE, MAP_SHARED, fd, bar_address[0][5]);
    if (ver_reg == MAP_FAILED) {
        perror("mmap:");
        ret = errno;
    }

    fpga_version = (uint32_t) (*(ver_reg + 7));

    if ((fpga_version != 0x00030100) && (fpga_version != 0x00030004)) {
        for (i = 0; i < MAX_BARS; i++) {
            ret = get_address(1, i);
            if (ret) {
                perror("Get_Bar_Addr:");
            }
        }
    }
    munmap(ver_reg, getpagesize());

    return SUCCESS;
}

uint8_t nand_dm_init() {
    int ret, fdi, version = 0;
    uint32_t *ver_reg;

    if ((fd = open("/dev/mem", O_RDWR)) < 0) {
        perror("open:");
        ret = fd;
    }
    ret = read_bar_address();
    if (ret) {
        close(fd);
        return FAILURE;
    }
    /*Passing respective BAR address to do mmap based on FPGA image Version*/
    if (fpga_version == 0x00030100) {
        ret = mmap_fpga_regs(fd, bar_address[0][2], bar_address[0][4]);
        CHIP_COUNT = 8;
    } else if (fpga_version == 0x00030004) {
        ret = mmap_fpga_regs(fd, bar_address[0][2], bar_address[0][4]);
        CHIP_COUNT = 2;
    } else if (fpga_version == 0x00020502) {
        ret = mmap_fpga_regs(fd, bar_address[1][2], bar_address[1][4]);
        CHIP_COUNT = 8;
    } else {
        printf("This FPGA image %02x.%02x.%02x is not supported. "
                " Upgrade to 02.05.02 or 03.00.04 or 03.01.00 image version\n",
                fpga_version & 0x00FF0000, fpga_version & 0x0000FF00,
                fpga_version & 0x000000FF);
        close(fd);
        return FAILURE;
    }
    close(fd);

    if (ret) {
        perror("mmap failed:");
        return FAILURE;
    }

    ret = read_mem_addr();
    if (ret) {
        perror("read_mem_addr:");
    }

    ret = virt_map();
    if (ret) {
        perror("virtual_mem_map:");
        return FAILURE;
    }

    init_dma_mgr();

    nand_running = 0;
    if (pthread_create(&io_completion, NULL, (void *) io_completer, NULL) != 0)
        log_err(" [mmgr: ERROR. nand completion thread not started.\n");

    /*Setting the feature to NAND_SYNC_MODE2 or ASYNC_MODE_5*/
    uint16_t cds_1[4] = {0, 0, 0, 0}, cds_2[4] = {0, 0, 0, 0},
    async[4] = {5, 0, 0, 0};
    uint16_t en_odt[4] = {0x20, 0, 0, 0}, s_mode2[4] ={0x22, 0, 0, 0}, s_mode1[4] = {0x21, 0, 0, 0};
    if (fpga_version == 0x00030004) {
        ret = nand_set_feature(0x10, 0x4, &cds_1);
        ret += nand_set_feature(0x80, 0x4, &cds_2);
        ret += nand_set_feature(0x2, 0x4, &en_odt);
        ret += nand_set_feature(0x1, 0x4, &s_mode2);
    } else if (fpga_version == 0x00020502) {
        ret = nand_set_feature(0x1, 0x4, &async);
    } else if (fpga_version == 0x00030100) {
        ret += nand_set_feature(0x1, 0x4, &s_mode2);
    }

    if (ret) {
        perror("NAND_SET_FEATURE:");
        return FAILURE;
    }

    return SUCCESS;
}

uint8_t nand_dm_deinit() {
    nand_running = 1;
    pthread_join(io_completion, NULL);
    io_completion = 0;
    virt_unmap();
    munmap(desc_tbl_addr, size[0] * getpagesize());
    munmap(ctrl_reg_addr, size[1] * getpagesize());
    return SUCCESS;
}