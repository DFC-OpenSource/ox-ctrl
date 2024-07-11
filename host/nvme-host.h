/* OX: Open-Channel NVM Express SSD Controller
 *
 *  - NVMe Host Interface (header)
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
 * 
 */

#include <ox-fabrics.h>

#ifndef NVME_HOST_H
#define NVME_HOST_H

/* NVMe block size (FOR NOW, ONLY 4096 IS SUPPORTED) */
#define NVMEH_BLK_SZ            4096

/* NVMe Identify Controller Data Structure */
struct nvme_id_ctrl {
    uint16_t    vid; 		/* PCI Vendor ID */
    uint16_t    ssvid; 		/* PCI Subsystem Vendor ID */
    uint8_t     sn[20]; 	/* Serial Number */
    uint8_t     mn[40]; 	/* Model Number */
    uint8_t     fr[8]; 		/* Firmware Revision */
    uint8_t     rab; 		/* Recommended Arbitration Burst */
    uint8_t     ieee[3];	/* IEEE OUI Identifier */
    uint8_t     cmic; 		/* Controller Multi-Path I/O and Namespace Sharing Cap */
    uint8_t     mdts; 		/* Maximum Data Transfer Size */
    uint16_t    cntlid; 	/* Controller ID */
    uint32_t    ver; 		/* Version */
    uint32_t    rtd3r; 		/* RTD3 Resume Latency */
    uint32_t    rtd3e; 		/* RTD3 Entry Latency */
    uint32_t    oaes; 		/* Optional Asynchronous Events Supported */
    uint32_t    ctratt; 	/* Controller Attributes */
    uint8_t     rsv[140];
    uint8_t     rsv255[16];
    uint16_t    oacs; 		/* Optional Admin Command Support */
    uint8_t     acl; 		/* Abort Command Limit */
    uint8_t     aerl; 		/* Asynchronous Event Request Limit */
    uint8_t     frmw;		/* Firmware Updates */
    uint8_t     lpa; 		/* Log Page Attributes */
    uint8_t     elpe; 		/* Error Log Page Entries */
    uint8_t     npss; 		/* Number of Power States Support */
    uint8_t     avscc; 		/* Admin Vendor Specific Command Configuration */
    uint8_t     apsta; 		/* Autonomous Power State Transition Attributes */
    uint16_t    wctemp; 	/* Warning Composite Temperature Threshold */
    uint16_t    cctemp; 	/* Critical Composite Temperature Threshold */
    uint16_t    mtfa; 		/* Maximum Time for Firmware Activation */
    uint32_t    hmpre; 		/* Host Memory Buffer Preferred Size */
    uint32_t    hmmin; 		/* Host Memory Buffer Minimum Size */
    uint8_t     tnvmcap[16]; 	/* Total NVM Capacity */
    uint8_t     unvmcap[16]; 	/* Unallocated NVM Capacity */
    uint32_t    rpmbs; 		/* Replay Protected Memory Block Support */
    uint32_t    rsv319;
    uint16_t    kas; 		/* Keep Alive Support */
    uint8_t     rsv511[190]; 	/*  */
    uint8_t     sqes; 		/* Submission Queue Entry Size */
    uint8_t     cqes; 		/* Completion Queue Entry Size */
    uint16_t    maxcmd; 	/* Maximum Outstanding Commands */
    uint32_t    nn; 		/* Number of Namespaces */
    uint16_t    oncs; 		/* Optional NVM Command Support */
    uint16_t    fuses; 		/* Fused Operation Support */
    uint8_t     fna; 		/* Format NVM Attributes */
    uint8_t     vwc; 		/* Volatile Write Cache */
    uint16_t    awun; 		/* Atomic Write Unit Normal */
    uint16_t    awupf;		/* Atomic Write Unit Power Fail */
    uint8_t     nvscc; 		/* NVM Vendor Specific Command Configuration */
    uint8_t     rsv531;
    uint16_t    acwu; 		/* Atomic Compare & Write Unit */
    uint16_t    rsv535;
    uint32_t    sgls; 		/* SGL Support */
    uint8_t     rsv767[228];
    uint8_t     subnqn[256]; 	/* NVM Subsystem NVMe Qualified Name */
    uint8_t     rsv1791[768];
    uint8_t     fabrics[256];
    NvmePSD     psd[32]; 	/* Power State Descriptors */
    uint8_t     vs[1024]; 	/* Vendor Specific */
};

/* NVMe Identify Namespace Data Structure */
struct nvme_id_ns {
	uint64_t    nsze; 	/* Namespace Size */
	uint64_t    ncap; 	/* Namespace Capacity */
	uint64_t    nuse; 	/* Namespace Utilization */
	uint8_t     nsfeat; 	/* Namespace Features */
	uint8_t     nlbaf; 	/* Number of LBA Formats */
	uint8_t     flbas; 	/* Formatted LBA Size */
	uint8_t     mc; 	/* Metadata Capabilities */
	uint8_t     dpc; 	/* End-to-end Data Protection Capabilities */
	uint8_t     dps; 	/* End-to-end Data Protection Type Settings */
        uint8_t     nmic; 	/* Namespace Multi-path I/O and NS Sharing Cap */
        uint8_t     rescap; 	/* Reservation Capabilities */
        uint8_t     fpi; 	/* Format Progress Indicator */
        uint8_t     rsv33;
        uint16_t    nawun; 	/* Namespace Atomic Write Unit Normal */
        uint16_t    nawupf; 	/* Namespace Atomic Write Unit Power Fail */
        uint16_t    nacwu; 	/* Namespace Atomic Compare & Write Unit */
        uint16_t    nabsn; 	/* Namespace Atomic Boundary Size Normal */
        uint16_t    nabo; 	/* Namespace Atomic Boundary Offset */
        uint16_t    nabspf; 	/* Namespace Atomic Boundary Size Power Fail */
        uint16_t    rsv47;
        uint64_t    nvmcap[2]; 	/* NVM Capacity */
        uint8_t     rsv103[40];
        uint8_t     nguid[16]; 	/* Namespace Globally Unique Identifier */
        uint8_t     eui64[8]; 	/* IEEE Extended Unique Identifier */
	union {
	    struct {
		uint16_t    ms;
		uint8_t     ds;
		uint8_t     rp;
	    } e;
	    uint32_t val;
	} lbaf;			/* LBA Format Support */
        uint8_t     rsv383[192];
	uint8_t     vs[3712]; 	/* Vendor Specific */
};

/* Prototype for the user defined callback function. Called when the NVMe
 * asynchronous functions return. 'ctx' is a pointer to the user data previously
 * provided. 'status' is 0 if the call succeeded. A positive value (NVME status)
 * is returned in failure.
 */
typedef void (nvme_host_callback_fn) (void *ctx, uint16_t status);

/* Initialize NVMe host (including the Fabrics) */
int  nvmeh_init (void);

/* Close the host, freeing all allocated resources */
void nvmeh_exit (void);

/**
 * Create an end-to-end NVMe over Fabrics queue between host and server. Each
 * queue has its own threads and TCP sockets.
 * 
 * @param qid - Queue ID. Use id 0 for admin queue (required). Positive values
 *              are I/O queues. It is recommended two I/O queues per network
 *              interface. e.g if you have 2 interfaces, create 1 admin queue
 *              and 4 I/O queues. A maximum of 64 queues are allowed, however,
 *              too many queues allocate lots of memory and the system may crash.
 * @return - return 0 if the queue is created and connected successfully. A
 *              positive value is returned if the call fails.
 */
int nvme_host_create_queue (uint16_t qid);


/**
 * Destroy a queue created by 'nvme_host_create_queue'. All allocated resources
 * are freed.
 * 
 * @param qid - Queue ID to be destroyed.
 */
void nvme_host_destroy_queue (uint16_t qid);


/**
 * Register a server network interface. This function must be called after
 * 'nvme_init'. Call this function several times for multiple interfaces.
 * 
 * @param addr - String containing the server IPv4 address. e.g "192.168.0.1".
 * @param port - 16-bit integer containing the server port.
 * @return - returns 0 in success and a negative value if NVMe is not
 *             initialized.
 */
int nvme_host_add_server_iface (const char *addr, uint16_t port);

/**
 * Reads data from an OX NVMe device.
 * 
 * @param buf - Memory pointer where data will be read into.
 * @param size - Data size to be read starting at 'slba'.
 * @param slba - Starting logical block address (each logical block is
 *                  4096 bytes in size, the only size supported for now).
 * @param cb - user defined callback function for command completion.
 * @param ctx - user defined context returned by the callback function.
 * @return returns 0 if the read has been submitted, or a negative value upon
 *          failure.
 */
int  nvmeh_read (uint8_t *buf, uint64_t size, uint64_t slba,
                                        nvme_host_callback_fn *cb, void *ctx);


/**
 * Writes data to an OX NVMe device.
 * 
 * @param buf - Memory pointer containing the date to be written.
 * @param size - Data size to be written starting at 'slba'.
 * @param slba - Starting logical block address (each logical block is
 *                  4096 bytes in size, the only size supported for now).
 * @param cb - user defined callback function for command completion.
 * @param ctx - user defined context returned by the callback function.
 * @return returns 0 if the write has been submitted, or a negative value upon
 *          failure.
 */
int  nvmeh_write (uint8_t *buf, uint64_t size, uint64_t slba,
                                        nvme_host_callback_fn *cb, void *ctx);

/**
 * Identify controller command.
 *
 * @param buf - Memory pointer to store the data structure. Must be 4k.
 */
int nvmeh_identify_ctrl (struct nvme_id_ctrl *buf);

/**
 * Identify namespace command.
 *
 * @param buf - Memory pointer to store the data structure. Must be 4k.
 * @param nsid - Namespace ID
 */
 int nvmeh_identify_ns (struct nvme_id_ns *buf, uint32_t nsid);

#endif /* NVME_HOST_H */

