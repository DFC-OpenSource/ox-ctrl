#ifndef NVME_H
#define NVME_H

#include <time.h>
#include <stdint.h>
#include <pthread.h>
#include <sys/queue.h>
#include <stdlib.h>
#include "lightnvm.h"
#include "ssd.h"

#define PCI_VENDOR_ID_INTEL     0x8086
#define PCI_VENDOR_ID_LNVM      0x1d1d

#define PCI_DEVICE_ID_LS2085    0x5845
#define PCI_DEVICE_ID_LNVM      0x1f1f

#define NVME_MAX_QS             2047 //PCI_MSIX_FLAGS_QSIZE -TODO
#define NVME_MAX_QUEUE_ENTRIES  0xffff
#define NVME_MAX_STRIDE         12
#define NVME_MAX_NUM_NAMESPACES 256
#define NVME_MAX_QUEUE_ES       0xf
#define NVME_MIN_CQUEUE_ES      0x4
#define NVME_MIN_SQUEUE_ES      0x6
#define NVME_SPARE_THRESHOLD    20
#define NVME_TEMPERATURE        0x143
#define NVME_OP_ABORTED         0xff

#define BDRV_SECTOR_BITS        9

#define NVME_NUM_QUEUES     64
#define NVME_MQ_MSGSIZE     8 /*Messages are integers indicating SQIDs*/
#define NVME_MQ_MAXMSG      NVME_NUM_QUEUES
#define NVME_MQ_NAME        "/nvme_mq"
#define SHADOW_REG_SZ       64
#define NVME_MAX_PRIORITY   4

#define NVME_KERNEL_PG_SIZE 4096

enum NvmeStatusCodes {
    NVME_SUCCESS                = 0x0000,
    NVME_INVALID_OPCODE         = 0x0001,
    NVME_INVALID_FIELD          = 0x0002,
    NVME_CID_CONFLICT           = 0x0003,
    NVME_DATA_TRAS_ERROR        = 0x0004,
    NVME_POWER_LOSS_ABORT       = 0x0005,
    NVME_INTERNAL_DEV_ERROR     = 0x0006,
    NVME_CMD_ABORT_REQ          = 0x0007,
    NVME_CMD_ABORT_SQ_DEL       = 0x0008,
    NVME_CMD_ABORT_FAILED_FUSE  = 0x0009,
    NVME_CMD_ABORT_MISSING_FUSE = 0x000a,
    NVME_INVALID_NSID           = 0x000b,
    NVME_CMD_SEQ_ERROR          = 0x000c,
    NVME_LBA_RANGE              = 0x0080,
    NVME_CAP_EXCEEDED           = 0x0081,
    NVME_NS_NOT_READY           = 0x0082,
    NVME_NS_RESV_CONFLICT       = 0x0083,
    NVME_INVALID_CQID           = 0x0100,
    NVME_INVALID_QID            = 0x0101,
    NVME_MAX_QSIZE_EXCEEDED     = 0x0102,
    NVME_ACL_EXCEEDED           = 0x0103,
    NVME_RESERVED               = 0x0104,
    NVME_AER_LIMIT_EXCEEDED     = 0x0105,
    NVME_INVALID_FW_SLOT        = 0x0106,
    NVME_INVALID_FW_IMAGE       = 0x0107,
    NVME_INVALID_IRQ_VECTOR     = 0x0108,
    NVME_INVALID_LOG_ID         = 0x0109,
    NVME_INVALID_FORMAT         = 0x010a,
    NVME_FW_REQ_RESET           = 0x010b,
    NVME_INVALID_QUEUE_DEL      = 0x010c,
    NVME_FID_NOT_SAVEABLE       = 0x010d,
    NVME_FID_NOT_NSID_SPEC      = 0x010f,
    NVME_FW_REQ_SUSYSTEM_RESET  = 0x0110,
    NVME_CONFLICTING_ATTRS      = 0x0180,
    NVME_INVALID_PROT_INFO      = 0x0181,
    NVME_WRITE_TO_RO            = 0x0182,
    NVME_INVALID_MEMORY_ADDRESS = 0x01C0,  /* Vendor extension. */
    NVME_WRITE_FAULT            = 0x0280,
    NVME_UNRECOVERED_READ       = 0x0281,
    NVME_E2E_GUARD_ERROR        = 0x0282,
    NVME_E2E_APP_ERROR          = 0x0283,
    NVME_E2E_REF_ERROR          = 0x0284,
    NVME_CMP_FAILURE            = 0x0285,
    NVME_ACCESS_DENIED          = 0x0286,
    NVME_MORE                   = 0x2000,
    NVME_DNR                    = 0x4000,
    NVME_NO_COMPLETE            = 0xffff,
};

enum NvmeAsyncEventRequest {
    NVME_AER_TYPE_ERROR                     = 0,
    NVME_AER_TYPE_SMART                     = 1,
    NVME_AER_TYPE_IO_SPECIFIC               = 6,
    NVME_AER_TYPE_VENDOR_SPECIFIC           = 7,
    NVME_AER_INFO_ERR_INVALID_SQ            = 0,
    NVME_AER_INFO_ERR_INVALID_DB            = 1,
    NVME_AER_INFO_ERR_DIAG_FAIL             = 2,
    NVME_AER_INFO_ERR_PERS_INTERNAL_ERR     = 3,
    NVME_AER_INFO_ERR_TRANS_INTERNAL_ERR    = 4,
    NVME_AER_INFO_ERR_FW_IMG_LOAD_ERR       = 5,
    NVME_AER_INFO_SMART_RELIABILITY         = 0,
    NVME_AER_INFO_SMART_TEMP_THRESH         = 1,
    NVME_AER_INFO_SMART_SPARE_THRESH        = 2,
};

enum NvmeFeatureIds {
    NVME_ARBITRATION                = 0x1,
    NVME_POWER_MANAGEMENT           = 0x2,
    NVME_LBA_RANGE_TYPE             = 0x3,
    NVME_TEMPERATURE_THRESHOLD      = 0x4,
    NVME_ERROR_RECOVERY             = 0x5,
    NVME_VOLATILE_WRITE_CACHE       = 0x6,
    NVME_NUMBER_OF_QUEUES           = 0x7,
    NVME_INTERRUPT_COALESCING       = 0x8,
    NVME_INTERRUPT_VECTOR_CONF      = 0x9,
    NVME_WRITE_ATOMICITY            = 0xa,
    NVME_ASYNCHRONOUS_EVENT_CONF    = 0xb,
    NVME_SOFTWARE_PROGRESS_MARKER   = 0x80
};

enum LogIdentifier {
    NVME_LOG_ERROR_INFO     = 0x01,
    NVME_LOG_SMART_INFO     = 0x02,
    NVME_LOG_FW_SLOT_INFO   = 0x03,
};

enum NvmeIdNsDps {
    DPS_TYPE_NONE   = 0,
    DPS_TYPE_1      = 1,
    DPS_TYPE_2      = 2,
    DPS_TYPE_3      = 3,
    DPS_TYPE_MASK   = 0x7,
    DPS_FIRST_EIGHT = 8,
};

enum NvmeQFlags {
    NVME_Q_PC           = 1,
    NVME_Q_PRIO_URGENT  = 0,
    NVME_Q_PRIO_HIGH    = 1,
    NVME_Q_PRIO_NORMAL  = 2,
    NVME_Q_PRIO_LOW     = 3,
};

enum NvmeIdCtrlOacs {
    NVME_OACS_SECURITY  = 1 << 0,
    NVME_OACS_FORMAT    = 1 << 1,
    NVME_OACS_FW        = 1 << 2,
    NVME_OACS_NS_MGMT   = 1 << 3,
    NVME_OACS_LNVM_DEV  = 1 << 4,
};

enum NvmeIdCtrlOncs {
    NVME_ONCS_COMPARE       = 1 << 0,
    NVME_ONCS_WRITE_UNCORR  = 1 << 1,
    NVME_ONCS_DSM           = 1 << 2,
    NVME_ONCS_WRITE_ZEROS   = 1 << 3,
    NVME_ONCS_FEATURES      = 1 << 4,
    NVME_ONCS_RESRVATIONS   = 1 << 5,
};

enum NvmeCapShift {
    CAP_MQES_SHIFT     = 0,
    CAP_CQR_SHIFT      = 16,
    CAP_AMS_SHIFT      = 17,
    CAP_TO_SHIFT       = 24,
    CAP_DSTRD_SHIFT    = 32,
    CAP_NSSRS_SHIFT    = 33,
    CAP_CSS_SHIFT      = 37,
    CAP_LIGHTNVM_SHIFT = 44,
    CAP_MPSMIN_SHIFT   = 48,
    CAP_MPSMAX_SHIFT   = 52,
};

enum NvmeCapMask {
    CAP_MQES_MASK      = 0xffff,
    CAP_CQR_MASK       = 0x1,
    CAP_AMS_MASK       = 0x3,
    CAP_TO_MASK        = 0xff,
    CAP_DSTRD_MASK     = 0xf,
    CAP_NSSRS_MASK     = 0x1,
    CAP_CSS_MASK       = 0xff,
    CAP_LIGHTNVM_MASK  = 0x1,
    CAP_MPSMIN_MASK    = 0xf,
    CAP_MPSMAX_MASK    = 0xf,
};

enum NvmeCcShift {
    CC_EN_SHIFT     = 0,
    CC_CSS_SHIFT    = 4,
    CC_MPS_SHIFT    = 7,
    CC_AMS_SHIFT    = 11,
    CC_SHN_SHIFT    = 14,
    CC_IOSQES_SHIFT = 16,
    CC_IOCQES_SHIFT = 20,
};

enum NvmeCcMask {
    CC_EN_MASK      = 0x1,
    CC_CSS_MASK     = 0x7,
    CC_MPS_MASK     = 0xf,
    CC_AMS_MASK     = 0x7,
    CC_SHN_MASK     = 0x3,
    CC_IOSQES_MASK  = 0xf,
    CC_IOCQES_MASK  = 0xf,
};

enum NvmeCmdDW0 {
    CMD_DW0_FUSE1   = (1 << 0),
    CMD_DW0_FUSE2   = (1 << 1),
    CMD_DW0_PSDT1   = (1 << 6),
    CMD_DW0_PSDT2   = (1 << 7)
};

#define NVME_CC_EN(cc)     ((cc >> CC_EN_SHIFT)     & CC_EN_MASK)
#define NVME_CC_CSS(cc)    ((cc >> CC_CSS_SHIFT)    & CC_CSS_MASK)
#define NVME_CC_MPS(cc)    ((cc >> CC_MPS_SHIFT)    & CC_MPS_MASK)
#define NVME_CC_AMS(cc)    ((cc >> CC_AMS_SHIFT)    & CC_AMS_MASK)
#define NVME_CC_SHN(cc)    ((cc >> CC_SHN_SHIFT)    & CC_SHN_MASK)
#define NVME_CC_IOSQES(cc) ((cc >> CC_IOSQES_SHIFT) & CC_IOSQES_MASK)
#define NVME_CC_IOCQES(cc) ((cc >> CC_IOCQES_SHIFT) & CC_IOCQES_MASK)

#define NVME_CAP_MQES(cap)  (((cap) >> CAP_MQES_SHIFT)   & CAP_MQES_MASK)
#define NVME_CAP_CQR(cap)   (((cap) >> CAP_CQR_SHIFT)    & CAP_CQR_MASK)
#define NVME_CAP_AMS(cap)   (((cap) >> CAP_AMS_SHIFT)    & CAP_AMS_MASK)
#define NVME_CAP_TO(cap)    (((cap) >> CAP_TO_SHIFT)     & CAP_TO_MASK)
#define NVME_CAP_DSTRD(cap) (((cap) >> CAP_DSTRD_SHIFT)  & CAP_DSTRD_MASK)
#define NVME_CAP_NSSRS(cap) (((cap) >> CAP_NSSRS_SHIFT)  & CAP_NSSRS_MASK)
#define NVME_CAP_CSS(cap)   (((cap) >> CAP_CSS_SHIFT)    & CAP_CSS_MASK)
#define NVME_CAP_LIGHTNVM(cap)(((cap)>>CAP_LIGHTNVM_SHIFT) & CAP_LIGHTNVM_MASK)
#define NVME_CAP_MPSMIN(cap)(((cap) >> CAP_MPSMIN_SHIFT) & CAP_MPSMIN_MASK)
#define NVME_CAP_MPSMAX(cap)(((cap) >> CAP_MPSMAX_SHIFT) & CAP_MPSMAX_MASK)

#define NVME_CAP_SET_MQES(cap, val)   (cap |= (uint64_t)(val & CAP_MQES_MASK)  \
		<< CAP_MQES_SHIFT)
#define NVME_CAP_SET_CQR(cap, val)    (cap |= (uint64_t)(val & CAP_CQR_MASK)   \
		<< CAP_CQR_SHIFT)
#define NVME_CAP_SET_AMS(cap, val)    (cap |= (uint64_t)(val & CAP_AMS_MASK)   \
		<< CAP_AMS_SHIFT)
#define NVME_CAP_SET_TO(cap, val)     (cap |= (uint64_t)(val & CAP_TO_MASK)    \
		<< CAP_TO_SHIFT)
#define NVME_CAP_SET_DSTRD(cap, val)  (cap |= (uint64_t)(val & CAP_DSTRD_MASK) \
		<< CAP_DSTRD_SHIFT)
#define NVME_CAP_SET_NSSRS(cap, val)  (cap |= (uint64_t)(val & CAP_NSSRS_MASK) \
		<< CAP_NSSRS_SHIFT)
#define NVME_CAP_SET_CSS(cap, val)    (cap |= (uint64_t)(val & CAP_CSS_MASK)   \
		<< CAP_CSS_SHIFT)
#define NVME_CAP_SET_LIGHTNVM(cap, val) (cap |= \
                    (uint64_t)(val & CAP_LIGHTNVM_MASK) << CAP_LIGHTNVM_SHIFT)
#define NVME_CAP_SET_MPSMIN(cap, val) (cap |= (uint64_t)(val & CAP_MPSMIN_MASK)\
		<< CAP_MPSMIN_SHIFT)
#define NVME_CAP_SET_MPSMAX(cap, val) (cap |= (uint64_t)(val & CAP_MPSMAX_MASK)\
		<< CAP_MPSMAX_SHIFT)

#define NVME_ID_NS_NSFEAT_THIN(nsfeat)      ((nsfeat & 0x1))
#define NVME_ID_NS_FLBAS_EXTENDED(flbas)    ((flbas >> 4) & 0x1)
#define NVME_ID_NS_FLBAS_INDEX(flbas)       ((flbas & 0xf))
#define NVME_ID_NS_MC_SEPARATE(mc)          ((mc >> 1) & 0x1)
#define NVME_ID_NS_MC_EXTENDED(mc)          ((mc & 0x1))
#define NVME_ID_NS_DPC_LAST_EIGHT(dpc)      ((dpc >> 4) & 0x1)
#define NVME_ID_NS_DPC_FIRST_EIGHT(dpc)     ((dpc >> 3) & 0x1)
#define NVME_ID_NS_DPC_TYPE_3(dpc)          ((dpc >> 2) & 0x1)
#define NVME_ID_NS_DPC_TYPE_2(dpc)          ((dpc >> 1) & 0x1)
#define NVME_ID_NS_DPC_TYPE_1(dpc)          ((dpc & 0x1))
#define NVME_ID_NS_DPC_TYPE_MASK            0x7

enum NvmeCstsShift {
    CSTS_RDY_SHIFT      = 0,
    CSTS_CFS_SHIFT      = 1,
    CSTS_SHST_SHIFT     = 2,
    CSTS_NSSRO_SHIFT    = 4,
};

enum NvmeCstsMask {
    CSTS_RDY_MASK   = 0x1,
    CSTS_CFS_MASK   = 0x1,
    CSTS_SHST_MASK  = 0x3,
    CSTS_NSSRO_MASK = 0x1,
};

enum NvmeCsts {
    NVME_CSTS_READY         = 1 << CSTS_RDY_SHIFT,
    NVME_CSTS_FAILED        = 1 << CSTS_CFS_SHIFT,
    NVME_CSTS_SHST_NORMAL   = 0 << CSTS_SHST_SHIFT,
    NVME_CSTS_SHST_PROGRESS = 1 << CSTS_SHST_SHIFT,
    NVME_CSTS_SHST_COMPLETE = 2 << CSTS_SHST_SHIFT,
    NVME_CSTS_NSSRO         = 1 << CSTS_NSSRO_SHIFT,
};

#define NVME_CSTS_RDY(csts)     ((csts >> CSTS_RDY_SHIFT)   & CSTS_RDY_MASK)
#define NVME_CSTS_CFS(csts)     ((csts >> CSTS_CFS_SHIFT)   & CSTS_CFS_MASK)
#define NVME_CSTS_SHST(csts)    ((csts >> CSTS_SHST_SHIFT)  & CSTS_SHST_MASK)
#define NVME_CSTS_NSSRO(csts)   ((csts >> CSTS_NSSRO_SHIFT) & CSTS_NSSRO_MASK)

enum NvmeAqaShift {
    AQA_ASQS_SHIFT  = 0,
    AQA_ACQS_SHIFT  = 16,
};

enum NvmeAqaMask {
    AQA_ASQS_MASK   = 0xfff,
    AQA_ACQS_MASK   = 0xfff,
};

#define NVME_AQA_ASQS(aqa) ((aqa >> AQA_ASQS_SHIFT) & AQA_ASQS_MASK)
#define NVME_AQA_ACQS(aqa) ((aqa >> AQA_ACQS_SHIFT) & AQA_ACQS_MASK)

#define NVME_CTRL_SQES_MIN(sqes) ((sqes) & 0xf)
#define NVME_CTRL_SQES_MAX(sqes) (((sqes) >> 4) & 0xf)
#define NVME_CTRL_CQES_MIN(cqes) ((cqes) & 0xf)
#define NVME_CTRL_CQES_MAX(cqes) (((cqes) >> 4) & 0xf)

#define NVME_ARB_AB(arb)    (arb & 0x7)
#define NVME_ARB_LPW(arb)   ((arb >> 8) & 0xff)
#define NVME_ARB_MPW(arb)   ((arb >> 16) & 0xff)
#define NVME_ARB_HPW(arb)   ((arb >> 24) & 0xff)

#define NVME_INTC_THR(intc)     (intc & 0xff)
#define NVME_INTC_TIME(intc)    ((intc >> 8) & 0xff)

#define NVME_SQ_FLAGS_PC(sq_flags)      (sq_flags & 0x1)
#define NVME_SQ_FLAGS_QPRIO(sq_flags)   ((sq_flags >> 1) & 0x3)

#define FREE_VALID(buf) do { if (buf) {free (buf); buf = NULL;} } while (0)
#define SAFE_CLOSE(fd) do { if (fd > 0) {close (fd); fd = 0;} } while (0)

#define NVME_CQ_FLAGS_PC(cq_flags)  (cq_flags & 0x1)
#define NVME_CQ_FLAGS_IEN(cq_flags) ((cq_flags >> 1) & 0x1)

enum NvmeSmartWarn {
    NVME_SMART_SPARE                  = 1 << 0,
    NVME_SMART_TEMPERATURE            = 1 << 1,
    NVME_SMART_RELIABILITY            = 1 << 2,
    NVME_SMART_MEDIA_READ_ONLY        = 1 << 3,
    NVME_SMART_FAILED_VOLATILE_MEDIA  = 1 << 4,
};

enum NvmeAdminCommands {
    NVME_ADM_CMD_DELETE_SQ      = 0x00,
    NVME_ADM_CMD_CREATE_SQ      = 0x01,
    NVME_ADM_CMD_GET_LOG_PAGE   = 0x02,
    NVME_ADM_CMD_DELETE_CQ      = 0x04,
    NVME_ADM_CMD_CREATE_CQ      = 0x05,
    NVME_ADM_CMD_IDENTIFY       = 0x06,
    NVME_ADM_CMD_ABORT          = 0x08,
    NVME_ADM_CMD_SET_FEATURES   = 0x09,
    NVME_ADM_CMD_GET_FEATURES   = 0x0a,
    NVME_ADM_CMD_ASYNC_EV_REQ   = 0x0c,
    NVME_ADM_CMD_ACTIVATE_FW    = 0x10,
    NVME_ADM_CMD_DOWNLOAD_FW    = 0x11,
    NVME_ADM_CMD_FORMAT_NVM     = 0x80,
    NVME_ADM_CMD_SECURITY_SEND  = 0x81,
    NVME_ADM_CMD_SECURITY_RECV  = 0x82,
    /*Vendor specific start here*/
};

enum {
    NVME_RW_LR                  = 1 << 15,
    NVME_RW_FUA                 = 1 << 14,
    NVME_RW_DSM_FREQ_UNSPEC     = 0,
    NVME_RW_DSM_FREQ_TYPICAL    = 1,
    NVME_RW_DSM_FREQ_RARE       = 2,
    NVME_RW_DSM_FREQ_READS      = 3,
    NVME_RW_DSM_FREQ_WRITES     = 4,
    NVME_RW_DSM_FREQ_RW         = 5,
    NVME_RW_DSM_FREQ_ONCE       = 6,
    NVME_RW_DSM_FREQ_PREFETCH   = 7,
    NVME_RW_DSM_FREQ_TEMP       = 8,
    NVME_RW_DSM_LATENCY_NONE    = 0 << 4,
    NVME_RW_DSM_LATENCY_IDLE    = 1 << 4,
    NVME_RW_DSM_LATENCY_NORM    = 2 << 4,
    NVME_RW_DSM_LATENCY_LOW     = 3 << 4,
    NVME_RW_DSM_SEQ_REQ         = 1 << 6,
    NVME_RW_DSM_COMPRESSED      = 1 << 7,
    NVME_RW_PRINFO_PRACT        = 1 << 13,
    NVME_RW_PRINFO_PRCHK_GUARD  = 1 << 12,
    NVME_RW_PRINFO_PRCHK_APP    = 1 << 11,
    NVME_RW_PRINFO_PRCHK_REF    = 1 << 10,
};

enum NvmeIoCommands {
    NVME_CMD_FLUSH              = 0x00,
    NVME_CMD_WRITE              = 0x01,
    NVME_CMD_READ               = 0x02,
    NVME_CMD_WRITE_UNCOR        = 0x04,
    NVME_CMD_COMPARE            = 0x05,
    NVME_CMD_WRITE_ZEROS        = 0x08,
    NVME_CMD_DSM                = 0x09,
};

typedef struct NvmeCmd {
    uint8_t     opcode;
    uint8_t     fuse;
    uint16_t    cid;
    uint32_t    nsid;
    uint64_t    res1;
    uint64_t    mptr;
    uint64_t    prp1;
    uint64_t    prp2;
    uint32_t    cdw10;
    uint32_t    cdw11;
    uint32_t    cdw12;
    uint32_t    cdw13;
    uint32_t    cdw14;
    uint32_t    cdw15;
} NvmeCmd;

typedef struct NvmeIdentify {
    uint8_t     opcode;
    uint8_t     flags;
    uint16_t    cid;
    uint32_t    nsid;
    uint64_t    rsvd2[2];
    uint64_t    prp1;
    uint64_t    prp2;
    uint8_t     cns;
    uint8_t     rsv15;
    uint16_t    cntid;
    uint32_t    rsvd11[5];
} NvmeIdentify;

typedef struct NvmeDeleteQ {
    uint8_t     opcode;
    uint8_t     flags;
    uint16_t    cid;
    uint32_t    rsvd1[9];
    uint16_t    qid;
    uint16_t    rsvd10;
    uint32_t    rsvd11[5];
} NvmeDeleteQ;

typedef struct NvmeCreateSq {
    uint8_t     opcode;
    uint8_t     flags;
    uint16_t    cid;
    uint32_t    rsvd1[5];
    uint64_t    prp1;
    uint64_t    rsvd8;
    uint16_t    sqid;
    uint16_t    qsize;
    uint16_t    sq_flags;
    uint16_t    cqid;
    uint32_t    rsvd12[4];
} NvmeCreateSq;

typedef struct NvmeCreateCq {
    uint8_t     opcode;
    uint8_t     flags;
    uint16_t    cid;
    uint32_t    rsvd1[5];
    uint64_t    prp1;
    uint64_t    rsvd8;
    uint16_t    cqid;
    uint16_t    qsize;
    uint16_t    cq_flags;
    uint16_t    irq_vector;
    uint32_t    rsvd12[4];
} NvmeCreateCq;

typedef struct NvmeRwCmd {
    uint8_t     opcode;
    uint8_t     flags;
    uint16_t    cid;
    uint32_t    nsid;
    uint64_t    rsvd2;
    uint64_t    mptr;
    uint64_t    prp1;
    uint64_t    prp2;
    uint64_t    slba;
    uint16_t    nlb;
    uint16_t    control;
    uint32_t    dsmgmt;
    uint32_t    reftag;
    uint16_t    apptag;
    uint16_t    appmask;
} NvmeRwCmd;

typedef struct vs_reg {
    uint8_t rsvd;
    uint8_t mnr;
    uint16_t mjr;
} vs_t;

typedef struct cc_reg {
    uint8_t en:1;
    uint8_t rsvd1:3;
    uint8_t iocss:3;
    uint8_t mps:4;
    uint8_t ams:3;
    uint8_t shn:2;
    uint8_t iosqes:4;
    uint8_t iocqes:4;
    uint8_t rsvd2:8;
} cconf_t;

typedef struct csts_reg {
    uint8_t rdy:1;
    uint8_t cfs:1;
    uint8_t shst:2;
    uint8_t nssro:1;
    uint8_t pp:1;
    uint32_t rsvd:26;
} csts_t;

typedef struct aqa_t {
    uint16_t asqs:12;
    uint8_t rsvd1:4;
    uint16_t acqs:12;
    uint8_t rsvd2:4;
} aqa_t;

typedef struct asq_reg {
    uint16_t rsvd:12;
    uint64_t asqb:52;
} asq_t;

typedef struct acq_reg {
    uint16_t rsvd:12;
    uint64_t acqb:52;
} acq_t;

typedef struct intms_reg {
    uint32_t ivms;
} intms_t;

typedef struct intmc_reg {
    uint32_t ivmc;
} intmc_t;

typedef struct nssr_reg {
    uint32_t nssrc;
} nssr_t;

typedef struct cap_reg {
    uint16_t    mqes:16;
    uint8_t     cqr:1;
    uint8_t     ams:2;
    uint8_t     rsvd1:5;
    uint8_t     to:8;
    uint8_t     dstrd:4;
    uint8_t     nssrs:1;
    uint8_t     Css:8;
    uint8_t     lnvm:1;
    int8_t     rsvd2:2;
    uint8_t     mpsmin:4;
    uint8_t     mpsmax:4;
    uint8_t     rsvd3:8;
} cap_t ;

typedef struct cmbloc_reg {
    uint8_t     bir:3;
    uint16_t	rsvd:9;
    uint32_t    ofst:20;
} cmbloc_t;

typedef struct cmbsz {
    uint8_t     sqs:1;
    uint8_t     cqs:1;
    uint8_t     lists:1;
    uint8_t     rds:1;
    uint8_t     wds:1;
    uint8_t		rsvd:3;
    uint8_t     szu:4;
    uint32_t    sz:20;
} cmbsz_t;

typedef struct NvmeRegs_bits {
    cap_t       cap;
    vs_t        vs;
    intms_t     intms;
    intmc_t     intmc;
    cconf_t     cc;
    uint32_t    rsvd1;
    csts_t      csts;
    nssr_t      nssrc;
    aqa_t       aqa;
    asq_t       asq;
    acq_t       acq;
    cmbloc_t    cmbloc;
    cmbsz_t     cmbsz;
} NvmeRegs_bits;

typedef struct NvmeRegs_vals {
    uint64_t    cap;
    uint32_t    vs;
    uint32_t    intms;
    uint32_t    intmc;
    uint32_t    cc;
    uint32_t    rsvd1;
    uint32_t    csts;
    uint32_t    nssrc;
    uint32_t    aqa;
    uint64_t    asq;
    uint64_t    acq;
    uint32_t    cmbloc;
    uint32_t    cmbsz;
} NvmeRegs_vals;

typedef union NvmeRegs {
    NvmeRegs_vals vBar;
    NvmeRegs_bits bBar;
} NvmeRegs;

typedef struct NvmeAerResult {
    uint8_t event_type;
    uint8_t event_info;
    uint8_t log_page;
    uint8_t resv;
} NvmeAerResult;

typedef struct NvmeAsyncEvent {
    TAILQ_ENTRY(NvmeAsyncEvent) entry;
    NvmeAerResult result;
} NvmeAsyncEvent;

typedef struct NvmeQSched {
    uint16_t     n_active_iosqs;
    uint64_t     shadow_regs[NVME_MAX_PRIORITY][2];
    uint64_t     mask_regs[NVME_MAX_PRIORITY][2];
    uint32_t     *iodbst_reg;
    uint32_t     SQID[NVME_MAX_PRIORITY];
    unsigned int prio_avail[NVME_MAX_PRIORITY];
    unsigned int prio_lvl_next_q[NVME_MAX_PRIORITY];
    uint32_t     round_robin_status_regs[4];
    int	     WRR;
} NvmeQSched;

typedef struct NvmeErrorLog {
    uint64_t    error_count;
    uint16_t    sqid;
    uint16_t    cid;
    uint16_t    status_field;
    uint16_t    param_error_location;
    uint64_t    lba;
    uint32_t    nsid;
    uint8_t     vs;
    uint8_t     resv[35];
} NvmeErrorLog;

typedef struct NvmeSmartLog {
    uint8_t     critical_warning;
    uint8_t     temperature[2];
    uint8_t     available_spare;
    uint8_t     available_spare_threshold;
    uint8_t     percentage_used;
    uint8_t     reserved1[26];
    uint64_t    data_units_read[2];
    uint64_t    data_units_written[2];
    uint64_t    host_read_commands[2];
    uint64_t    host_write_commands[2];
    uint64_t    controller_busy_time[2];
    uint64_t    power_cycles[2];
    uint64_t    power_on_hours[2];
    uint64_t    unsafe_shutdowns[2];
    uint64_t    media_errors[2];
    uint64_t    number_of_error_log_entries[2];
    uint8_t     reserved2[320];
} NvmeSmartLog;

typedef struct NvmeFwSlotInfoLog {
    uint8_t     afi;
    uint8_t     reserved1[7];
    uint8_t     frs1[8];
    uint8_t     frs2[8];
    uint8_t     frs3[8];
    uint8_t     frs4[8];
    uint8_t     frs5[8];
    uint8_t     frs6[8];
    uint8_t     frs7[8];
    uint8_t     reserved2[448];
} NvmeFwSlotInfoLog;

typedef struct NvmeCqe {
    union {
        struct {
            uint32_t    result;
            uint32_t    rsvd;
        } n;
        uint64_t    res64;
    };
    uint16_t    sq_head;
    uint16_t    sq_id;
    uint16_t    cid;
    uint16_t    status;
} NvmeCqe;

typedef struct NvmeSQ {
    TAILQ_ENTRY(NvmeSQ) entry;
    struct NvmeCtrl     *ctrl;
    uint8_t             phys_contig;
    uint8_t             arb_burst;
    uint16_t            sqid;
    uint16_t            cqid;
    uint32_t            head;
    uint32_t            tail;
    uint32_t            size;
    uint64_t            dma_addr;
    uint64_t            completed;
    uint64_t            *prp_list;
    struct NvmeRequest  *io_req;
    TAILQ_HEAD (sq_reqhead, NvmeRequest) req_list;
    TAILQ_HEAD (sq_outreqhead, NvmeRequest) out_req_list;
    /* Mapped memory location where the tail pointer is stored by the guest */
    uint64_t            db_addr;
    uint64_t            eventidx_addr;
    int                 fd_qmem;
    enum NvmeQFlags     prio;
    uint64_t            posted;
} NvmeSQ;

typedef struct NvmeCQ {
    struct NvmeCtrl     *ctrl;
    uint8_t             phys_contig;
    uint8_t             phase;
    uint16_t            cqid;
    uint16_t            irq_enabled;
    uint32_t            head;
    uint32_t            tail;
    uint32_t            vector;
    uint32_t            size;
    uint64_t            dma_addr;
    uint64_t            *prp_list;
    TAILQ_HEAD (cqsqhead, NvmeSQ)         sq_list;
    TAILQ_HEAD (cqreqhead, NvmeRequest)   req_list;
    /* Mapped memory location where the head pointer is stored by the guest */
    uint64_t            db_addr;
    uint64_t            eventidx_addr;
    int                 fd_qmem;
    volatile uint8_t    hold_sqs;
} NvmeCQ;

typedef struct NvmeLBAF {
	uint16_t    ms;
	uint8_t     ds;
	uint8_t     rp;
} NvmeLBAF;

typedef struct NvmeIdNs {
	uint64_t    nsze; /* Namespace Size */
	uint64_t    ncap; /* Namespace Capacity */
	uint64_t    nuse; /* Namespace Utilization */
	uint8_t     nsfeat; /* Namespace Features */
	uint8_t     nlbaf; /* Number of LBA Formats */
	uint8_t     flbas; /* Formatted LBA Size */
	uint8_t     mc; /* Metadata Capabilities */
	uint8_t     dpc; /* End-to-end Data Protection Capabilities */
	uint8_t     dps; /* End-to-end Data Protection Type Settings */
        uint8_t     nmic; /* Namespace Multi-path I/O and NS Sharing Cap */
        uint8_t     rescap; /* Reservation Capabilities */
        uint8_t     fpi; /* Format Progress Indicator */
        uint8_t     rsv33;
        uint16_t    nawun; /* Namespace Atomic Write Unit Normal */
        uint16_t    nawupf; /* Namespace Atomic Write Unit Power Fail */
        uint16_t    nacwu; /* Namespace Atomic Compare & Write Unit */
        uint16_t    nabsn; /* Namespace Atomic Boundary Size Normal */
        uint16_t    nabo; /* Namespace Atomic Boundary Offset */
        uint16_t    nabspf; /* Namespace Atomic Boundary Size Power Fail */
        uint16_t    rsv47;
        uint64_t    nvmcap[2]; /* NVM Capacity */
        uint8_t     rsv103[40];
        uint8_t     nguid[16]; /* Namespace Globally Unique Identifier */
        uint8_t     eui64[8]; /* IEEE Extended Unique Identifier */
        NvmeLBAF    lbaf[16]; /* LBA Format Support */
        uint8_t     rsv383[192];
	uint8_t     vs[3712]; /* Vendor Specific */
} NvmeIdNs;

typedef struct NvmeRangeType {
	uint8_t     type;
	uint8_t     attributes;
	uint8_t     rsvd2[14];
	uint64_t    slba;
	uint64_t    nlb;
	uint8_t     guid[16];
	uint8_t     rsvd48[16];
} NvmeRangeType;

typedef struct NvmeNamespace {
    struct NvmeCtrl *ctrl;
    NvmeIdNs        id_ns;
    NvmeRangeType   lba_range[64];
    unsigned long   *util;
    unsigned long   *uncorrectable;
    uint32_t        id;
    uint64_t        ns_blks;
    int64_t         start_block;
    uint64_t        meta_start_offset;
} NvmeNamespace;

typedef struct NvmeRequest {
    TAILQ_ENTRY(NvmeRequest) entry;
    struct NvmeSQ            *sq;
    struct NvmeNamespace     *ns;
    struct NvmeCmd           *cmd;
    NvmeCqe                  cqe;
    uint16_t                 status;
    uint64_t                 slba;
    uint16_t                 is_write;
    uint16_t                 nlb;
    uint16_t                 ctrl;
    uint64_t                 meta_size;
    uint64_t                 mptr;
    void                     *meta_buf;
    struct nvm_io_cmd        nvm_io;
    uint8_t                  lba_index;

#if LIGHTNVM
    uint64_t                 lightnvm_slba;
    uint64_t                 *lightnvm_ppa_list;
#endif /* LIGHTNVM */
} NvmeRequest;

typedef struct NvmeFeatureVal {
    uint32_t    arbitration;
    uint32_t    power_mgmt;
    uint32_t    temp_thresh;
    uint32_t    err_rec;
    uint32_t    volatile_wc;
    uint32_t    num_queues;
    uint32_t    int_coalescing;
    uint32_t    *int_vector_config;
    uint32_t    write_atomicity;
    uint32_t    async_config;
    uint32_t    sw_prog_marker;
} NvmeFeatureVal;

typedef struct NvmePSD {
	uint16_t    mp;
	uint16_t    reserved;
	uint32_t    enlat;
	uint32_t    exlat;
	uint8_t     rrt;
	uint8_t     rrl;
	uint8_t     rwt;
	uint8_t     rwl;
	uint8_t     resv[16];
} NvmePSD;

typedef struct NvmeIdCtrl {
    uint16_t    vid; /* PCI Vendor ID */
    uint16_t    ssvid; /* PCI Subsystem Vendor ID */
    uint8_t     sn[20]; /* Serial Number */
    uint8_t     mn[40]; /* Model Number */
    uint8_t     fr[8]; /* Firmware Revision */
    uint8_t     rab; /* Recommended Arbitration Burst */
    uint8_t     ieee[3]; /* IEEE OUI Identifier */
    uint8_t     cmic; /* Controller Multi-Path I/O and Namespace Sharing Cap */
    uint8_t     mdts; /* Maximum Data Transfer Size */
    uint16_t    cntlid; /* Controller ID */
    uint32_t    ver; /* Version */
    uint32_t    rtd3r; /* RTD3 Resume Latency */
    uint32_t    rtd3e; /* RTD3 Entry Latency */
    uint32_t    oaes; /* Optional Asynchronous Events Supported */
    uint32_t    ctratt; /* Controller Attributes */
    uint8_t     rsv[140];
    uint8_t     rsv255[16];
    uint16_t    oacs; /* Optional Admin Command Support */
    uint8_t     acl; /* Abort Command Limit */
    uint8_t     aerl; /* Asynchronous Event Request Limit */
    uint8_t     frmw; /* Firmware Updates */
    uint8_t     lpa; /* Log Page Attributes */
    uint8_t     elpe; /* Error Log Page Entries */
    uint8_t     npss; /* Number of Power States Support */
    uint8_t     avscc; /* Admin Vendor Specific Command Configuration */
    uint8_t     apsta; /* Autonomous Power State Transition Attributes */
    uint16_t    wctemp; /* Warning Composite Temperature Threshold */
    uint16_t    cctemp; /* Critical Composite Temperature Threshold */
    uint16_t    mtfa; /* Maximum Time for Firmware Activation */
    uint32_t    hmpre; /* Host Memory Buffer Preferred Size */
    uint32_t    hmmin; /* Host Memory Buffer Minimum Size */
    uint8_t     tnvmcap[16]; /* Total NVM Capacity */
    uint8_t     unvmcap[16]; /* Unallocated NVM Capacity */
    uint32_t    rpmbs; /* Replay Protected Memory Block Support */
    uint32_t    rsv319;
    uint16_t    kas; /* Keep Alive Support */
    uint8_t     rsv511[190]; /*  */
    uint8_t     sqes; /* Submission Queue Entry Size */
    uint8_t     cqes; /* Completion Queue Entry Size */
    uint16_t    maxcmd; /* Maximum Outstanding Commands */
    uint32_t    nn; /* Number of Namespaces */
    uint16_t    oncs; /* Optional NVM Command Support */
    uint16_t    fuses; /* Fused Operation Support */
    uint8_t     fna; /* Format NVM Attributes */
    uint8_t     vwc; /* Volatile Write Cache */
    uint16_t    awun; /* Atomic Write Unit Normal */
    uint16_t    awupf; /* Atomic Write Unit Power Fail */
    uint8_t     nvscc; /* NVM Vendor Specific Command Configuration */
    uint8_t     rsv531;
    uint16_t    acwu; /* Atomic Compare & Write Unit */
    uint16_t    rsv535;
    uint32_t    sgls; /* SGL Support */
    uint8_t     rsv767[228];
    uint8_t     subnqn[256]; /* NVM Subsystem NVMe Qualified Name */
    uint8_t     rsv1791[768];
    uint8_t     fabrics[256];
    NvmePSD     psd[32]; /* Power State Descriptors */
    uint8_t     vs[1024]; /* Vendor Specific */
} NvmeIdCtrl;

typedef struct NvmeBar {
    uint64_t    cap;
    uint32_t    vs;
    uint32_t    intms;
    uint32_t    intmc;
    uint32_t    cc;
    uint32_t    rsvd1;
    uint32_t    csts;
    uint32_t    nssrc;
    uint32_t    aqa;
    uint64_t    asq;
    uint64_t    acq;
    uint32_t    cmbloc;
    uint32_t    cmbsz;
} NvmeBar;

typedef struct NvmeStats {
    uint64_t    nr_bytes_read;
    uint64_t    nr_bytes_written;
    uint64_t    total_time_ns;
    uint64_t    tot_num_AdminCmd;
    uint64_t    tot_num_IOCmd;
    uint64_t    tot_num_ReadCmd;
    uint64_t    tot_num_WriteCmd;
    uint64_t    num_active_queues;
} NvmeStats;

typedef struct NvmeCtrl {
    struct nvm_memory_region   iomem;
    struct nvm_memory_region   ctrl_mem;
    NvmeBar                     bar;
    NvmeRegs                    nvme_regs;
    NvmeQSched                  qsched;
    NvmeStats                   stat;

    time_t      start_time;
    uint16_t    temperature;
    uint16_t    page_size;
    uint16_t    page_bits;
    uint16_t    max_prp_ents;
    uint16_t    cqe_size;
    uint16_t    sqe_size;
    uint32_t    reg_size;
    uint32_t    num_namespaces;
    uint32_t    num_queues;
    uint32_t    max_q_ents;
    uint64_t    *ns_size;
    uint8_t     db_stride;
    uint8_t     elp_index;
    uint8_t     error_count;
    uint8_t     cqr;
    uint8_t     max_sqes;
    uint8_t     max_cqes;
    uint8_t     meta;
    uint8_t     mc;
    uint8_t     dpc;
    uint8_t     dps;
    uint8_t     nlbaf;
    uint8_t     extended;
    uint8_t     lba_index;
    uint8_t     mpsmin;
    uint8_t     mpsmax;
    uint8_t     intc;
    uint8_t     intc_thresh;
    uint8_t     intc_time;
    uint8_t     outstanding_aers;
    uint8_t     temp_warn_issued;
    uint8_t     num_errors;
    uint8_t     cqes_pending;
    uint16_t    vid;
    uint16_t    did;
    uint16_t    cmb;
    uint32_t    cmbsz;
    uint32_t    cmbloc;
    uint8_t     *cmbuf;
    int         mq_txid;
    int         mq_rxid;

    char            *serial;
    NvmeErrorLog    *elpes;
    NvmeRequest     **aer_reqs;
    NvmeNamespace   *namespaces;
    NvmeSQ          **sq;
    NvmeCQ          **cq;
    NvmeSQ          admin_sq;
    NvmeCQ          admin_cq;
    NvmeFeatureVal  features;
    NvmeIdCtrl      id_ctrl;
    uint8_t         running;
    uint8_t         aer_mask;
    TAILQ_HEAD (ctrl_aerhead, NvmeAsyncEvent)   aer_queue;
    pthread_spinlock_t                          qs_req_spin;
    pthread_spinlock_t                          aer_req_spin;
    pthread_mutex_t                             req_mutex;

#if LIGHTNVM
    LnvmCtrl     lightnvm_ctrl;
#endif /* LIGHTNVM */
} NvmeCtrl;

void     nvme_exit(void);
uint8_t  nvme_write_to_host(void *, uint64_t, ssize_t);
uint8_t  nvme_read_from_host(void *, uint64_t, ssize_t);
uint16_t nvme_init_cq (NvmeCQ *, NvmeCtrl *, uint64_t, uint16_t, uint16_t,
        uint16_t, uint16_t, int);
uint16_t nvme_init_sq (NvmeSQ *, NvmeCtrl *, uint64_t, uint16_t, uint16_t,
        uint16_t, enum NvmeQFlags, int);
void     nvme_enqueue_req_completion (NvmeCQ *, NvmeRequest *);
void     nvme_post_cqes (void *);
int      nvme_check_cqid (NvmeCtrl *, uint16_t);
int      nvme_check_sqid (NvmeCtrl *, uint16_t);
void     nvme_free_sq (NvmeSQ *, NvmeCtrl *);
void     nvme_free_cq (NvmeCQ *, NvmeCtrl *);
void     nvme_addr_read (NvmeCtrl *, uint64_t, void *, int);
void     nvme_addr_write (NvmeCtrl *, uint64_t, void *, int);
void     nvme_enqueue_event (NvmeCtrl *, uint8_t, uint8_t, uint8_t);
void     nvme_rw_cb (void *);

/* NVMe Admin cmd */
uint16_t nvme_identify (NvmeCtrl *, NvmeCmd *);
uint16_t nvme_del_sq (NvmeCtrl *, NvmeCmd *);
uint16_t nvme_create_sq (NvmeCtrl *, NvmeCmd *);
uint16_t nvme_del_cq (NvmeCtrl *, NvmeCmd *);
uint16_t nvme_create_cq (NvmeCtrl *, NvmeCmd *);
uint16_t nvme_set_feature (NvmeCtrl *, NvmeCmd *, NvmeRequest *);
uint16_t nvme_get_feature (NvmeCtrl *, NvmeCmd *, NvmeRequest *);
uint16_t nvme_get_log(NvmeCtrl *, NvmeCmd *);
uint16_t nvme_async_req (NvmeCtrl *, NvmeCmd *, NvmeRequest *);
uint16_t nvme_format (NvmeCtrl *, NvmeCmd *);
uint16_t nvme_abort_req (NvmeCtrl *, NvmeCmd *, uint32_t *);

/* NVMe IO cmd */
uint16_t nvme_write_uncor(NvmeCtrl *,NvmeNamespace *,NvmeCmd *,NvmeRequest *);
uint16_t nvme_dsm (NvmeCtrl *, NvmeNamespace *, NvmeCmd *, NvmeRequest *);
uint16_t nvme_flush(NvmeCtrl *, NvmeNamespace *, NvmeCmd *, NvmeRequest *);
uint16_t nvme_compare(NvmeCtrl *, NvmeNamespace *, NvmeCmd *, NvmeRequest *);
uint16_t nvme_write_zeros(NvmeCtrl *,NvmeNamespace *,NvmeCmd *,NvmeRequest *);
uint16_t nvme_rw (NvmeCtrl *, NvmeNamespace *, NvmeCmd *, NvmeRequest *);

#if LIGHTNVM
/* LNVM functions */
int     lnvm_init(NvmeCtrl *);
uint8_t lnvm_dev(NvmeCtrl *);
void    lnvm_set_default(LnvmCtrl *);
void    lightnvm_exit(NvmeCtrl *);
void    lnvm_init_id_ctrl(LnvmIdCtrl *);

/* LNVM Admin cmd */
uint16_t lnvm_identity(NvmeCtrl *, NvmeCmd *);
uint16_t lnvm_get_l2p_tbl(NvmeCtrl *, NvmeCmd *, NvmeRequest *);
uint16_t lnvm_get_bb_tbl(NvmeCtrl *, NvmeCmd *, NvmeRequest *);
uint16_t lnvm_set_bb_tbl(NvmeCtrl *, NvmeCmd *, NvmeRequest *);

/* LNVM IO cmd */
uint16_t lnvm_erase_sync(NvmeCtrl *, NvmeNamespace *, NvmeCmd *, NvmeRequest *);
uint16_t lnvm_rw(NvmeCtrl *, NvmeNamespace *, NvmeCmd *, NvmeRequest *);
#endif /* LIGHTNVM */

#endif /* NVME_H */
