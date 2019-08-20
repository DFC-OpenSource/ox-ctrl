/* OX: Open-Channel NVM Express SSD Controller
 * 
 *  - NVM over Fabrics Specification (header)
 *
 * Copyright 2016 IT University of Copenhagen
 * 
 * Modified by Ivan Luiz Picoli <ivpi@itu.dk>
 * This file has been modified from the QEMU project NVMe device.
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

#ifndef NVMEF_H
#define NVMEF_H

/* Timeout 10 sec */
#define NVMEF_RETRY         50000
#define NVMEF_RETRY_DELAY   200

#define NVMEF_ICDOFF    16
#define NVMEF_SGL_SZ    256 /* Space for SGL after command in capsule */
#define NVMEF_DATA_OFF  320 /* Offset where data starts in capsule */

enum {
    NVMEF_CMD_PROP_SET  = 0x00,
    NVMEF_CMD_CONNECT   = 0x01,
    NVMEF_CMD_PROP_GET  = 0x04,
    NVMEF_CMD_AUTH_SEND = 0x05,
    NVMEF_CMD_AUTH_RECV = 0x06
};

typedef struct NvmefCmd {
    uint8_t     opcode;
    uint8_t     rsvd1;
    uint16_t    cid;
    uint8_t     fctype;
    uint8_t     rsvd2[35];
    uint8_t     fabrics[24];
} NvmefCmd;

typedef struct NvmefConnect {
    uint8_t     opcode;
    uint8_t     rsvd1;
    uint16_t    cid;
    uint8_t     fctype;
    uint8_t     rsvd2[19];
    uint8_t     sgl1[16];
    uint16_t    recfmt;
    uint16_t    qid;
    uint16_t    sqsize;
    uint8_t     cattr;
    uint8_t     rsvd3;
    uint32_t    kato;
    uint8_t     rsvd4[12];
} NvmefConnect;

typedef struct NvmefConnectData {
    uint8_t     hostid[16];
    uint16_t    cntlid;
    uint8_t     rsvd1[238];
    uint8_t     subnqn[256];
    uint8_t     hostnqn[256];
    uint8_t     rsvd2[256];
} NvmefConnectData;

typedef struct NvmefPropSetGet {
    uint8_t     opcode;
    uint8_t     rsvd1;
    uint16_t    cid;
    uint8_t     fctype;
    uint8_t     rsvd2[35];
    struct {
        uint8_t size    : 3;
        uint8_t rsvd2   : 5;
    } attrib;
    uint8_t     rsvd3[3];
    uint32_t    ofst;
    union {
        struct {
            uint64_t val;
            uint64_t rsvd4;
        } set;
        uint8_t rsvd4[16];
    };
} NvmefPropSetGet;

#endif  /* NVMEF_H */