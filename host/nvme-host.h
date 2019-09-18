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
 * @param buf - Memory pointer containing the data to be written.
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


#endif /* NVME_HOST_H */

