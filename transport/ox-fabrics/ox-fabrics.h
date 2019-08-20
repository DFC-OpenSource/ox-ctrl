/* OX: Open-Channel NVM Express SSD Controller
 *
 *  - OX NVMe over Fabrics (header) 
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

#ifndef OX_FABRICS_H
#define OX_FABRICS_H

#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <nvme.h>
#include <nvmef.h>

#define OXF_DEBUG       0

/* Timeout 10 sec */
#define OXF_QUEUE_TO    10000000

/* Timeout 10 sec */
#define OXF_RETRY       50000
#define OXF_RETRY_DELAY 200

#define OXF_UDP         1
#define OXF_TCP         2
#define OXF_PROTOCOL    OXF_TCP

#define OXF_REMOTE      0
#define OXF_FULL_IFACES 0

#if OXF_REMOTE
#define OXF_ADDR_1       "192.168.0.2"
#define OXF_ADDR_2       "192.168.1.2"
#define OXF_ADDR_3       "192.168.2.2"
#define OXF_ADDR_4       "192.168.3.2"
#else
#define OXF_ADDR_1       "127.0.0.1"
#define OXF_ADDR_2       "127.0.0.1"
#define OXF_ADDR_3       "127.0.0.1"
#define OXF_ADDR_4       "127.0.0.1"
#endif

#define OXF_PORT_1        35500
#define OXF_PORT_2        35501
#define OXF_PORT_3        35502
#define OXF_PORT_4        35503

#define OXF_SERVER_MAX_CON  64
#define OXF_CLIENT_MAX_CON  64
#define OXF_MAX_DGRAM       65507   /* Max UDP datagram size */
#define OXF_RCV_TO          1
#define OXF_QUEUE_SIZE      2047

#define OXF_BLK_SIZE        4096

enum oxf_capsule_types {
    OXF_ACK_BYTE    = 0x0c, /* Acknowledgement */
    OXF_CON_BYTE    = 0x1c, /* Connect command */
    OXF_DIS_BYTE    = 0x2c, /* Disconnect command */
    OXF_CMD_BYTE    = 0x3c, /* Submission queue entry */
    OXF_CQE_BYTE    = 0x4c, /* Completion queue entry */
    OXF_RDMA_BYTE   = 0x5c  /* RDMA packet */
};

#define OXF_CAPSULE_SZ      65504
#define OXF_SQC_MAX_DATA    65184 /* Data in a SQ capsule. Refer to NVMEF_DATA_OFF */
#define OXF_CQC_MAX_DATA    65488 /* Data in a CQ capsule */
#define OXF_NVME_CMD_SZ     64
#define OXF_NVME_CQE_SZ     16
#define OXF_FAB_HEADER_SZ   3
#define OXF_FAB_CMD_SZ      OXF_NVME_CMD_SZ + OXF_FAB_HEADER_SZ
#define OXF_FAB_CQE_SZ      OXF_NVME_CQE_SZ + OXF_FAB_HEADER_SZ
#define OXF_FAB_CAPS_SZ     OXF_CAPSULE_SZ + OXF_FAB_HEADER_SZ

struct nvme_sgl_desc {
    union {
        struct {
            uint64_t    addr;
            uint32_t    length;
            uint8_t     rsvd[3];
        }  __attribute__((packed)) data;
        struct {
            uint64_t    addr;
            uint8_t     length[3];
            uint32_t    key;
        }  __attribute__((packed)) keyed;
        struct {
            uint64_t    addr;
            uint16_t    length;
            uint8_t     key[5];
        }  __attribute__((packed)) keyed5;
        /* Add other types */
        uint8_t     specific[15];
    };
    uint8_t     subtype : 4;
    uint8_t     type    : 4;
} __attribute__((packed));

struct nvme_cmd {
    uint8_t     opcode;
    uint8_t     fuse : 2;
    uint8_t     rsvd : 4;
    uint8_t     psdt : 2;
    uint16_t    cid;
    uint32_t    nsid;
    uint64_t    res1;
    uint64_t    mptr;
    union {
        struct nvme_sgl_desc sgl;
        struct {
            uint64_t         prp1;
            uint64_t         prp2;
        };
    };
    uint32_t    cdw10;
    uint32_t    cdw11;
    uint32_t    cdw12;
    uint32_t    cdw13;
    uint32_t    cdw14;
    uint32_t    cdw15;
} __attribute__((packed));

struct nvme_cqe {
    uint32_t    result;
    uint32_t    rsvd;
    uint16_t    sq_head;
    uint16_t    sq_id;
    uint16_t    cid;
    uint16_t    status;
} __attribute__((packed));

struct nvmef_capsule_sq {
    struct nvme_cmd cmd;
    uint8_t         sgl[NVMEF_SGL_SZ];
    uint8_t         data[OXF_SQC_MAX_DATA];
} __attribute__((packed));

struct nvmef_capsule_cq {
    struct nvme_cqe cqe;
    uint8_t         data[OXF_CQC_MAX_DATA];
} __attribute__((packed));

struct oxf_capsule_sq {
    uint8_t                 type;
    uint16_t                size;
    struct nvmef_capsule_sq sqc;
    struct nvmef_capsule_cq cqc;
} __attribute__((packed));

struct oxf_capsule_cq {
    uint8_t                 type;
    uint16_t                size;
    struct nvmef_capsule_cq cqc;
} __attribute__((packed));

typedef void (oxf_rcv_fn) (uint32_t size, void *data, void *recv_cli);
typedef void (oxf_rcv_reply_fn) (uint32_t size, void *data);
typedef void (oxf_callback_fn) (void *ctx, struct nvme_cqe *cqe);

typedef void (oxf_host_callback_fn) (void *ctx, uint16_t status);

struct oxf_client;
struct oxf_server;

struct oxf_con_addr {
    char        addr[16];
    uint16_t    port;
};

struct oxf_server_con {
        struct sockaddr_in   addr;
        struct oxf_con_addr  haddr; /* Human readable address */
	struct oxf_server   *server;
        oxf_rcv_fn          *rcv_fn;
        pthread_t            tid;
	uint16_t             cid;
        uint8_t              running;
        int                  active_cli[OXF_SERVER_MAX_CON];
        pthread_t            cli_tid[OXF_SERVER_MAX_CON];
	int                  sock_fd;
};

struct oxf_client_con {
        struct sockaddr_in   addr;
	struct oxf_client   *client;
	uint16_t             cid;
	int                  sock_fd;
        pthread_t            recv_th;
        oxf_rcv_reply_fn    *recv_fn;
        uint8_t              running;
};

/* SERVER */

typedef void (oxf_svr_unbind) (struct oxf_server_con *con);
typedef int  (oxf_svr_conn_start) (struct oxf_server_con *con, oxf_rcv_fn *fn);
typedef void (oxf_svr_conn_stop) (struct oxf_server_con *con);
typedef int  (oxf_svr_reply) (struct oxf_server_con *con, const void *buf,
                                            uint32_t size, void *recv_client);
typedef struct oxf_server_con *(oxf_svr_bind) (struct oxf_server *server,
                            uint16_t conn_id, const char *addr, uint16_t port);

struct oxf_server_ops {
    oxf_svr_bind          *bind;
    oxf_svr_unbind        *unbind;
    oxf_svr_conn_start    *start;
    oxf_svr_conn_stop     *stop;
    oxf_svr_reply         *reply;
};

struct oxf_server {
        struct oxf_con_addr   ifaces[OXF_SERVER_MAX_CON];
	struct oxf_server_con *connections[OXF_SERVER_MAX_CON + 1];
        struct oxf_server_ops *ops;
	uint16_t n_con;
        uint16_t n_ifaces;
};

struct oxf_server *oxf_udp_server_init (void);
void               oxf_udp_server_exit (struct oxf_server *server);
struct oxf_server *oxf_tcp_server_init (void);
void               oxf_tcp_server_exit (struct oxf_server *server);

/* CLIENT */

typedef void               (oxf_cli_disconnect) (struct oxf_client_con *con);
typedef int                (oxf_cli_send) (struct oxf_client_con *con,
                                                uint32_t size, const void *buf);
typedef struct oxf_client_con *(oxf_cli_connect) (struct oxf_client *client,
      uint16_t cid, const char *addr, uint16_t port, oxf_rcv_reply_fn *recv_fn);

struct oxf_client_ops {
    oxf_cli_connect     *connect;
    oxf_cli_disconnect  *disconnect;
    oxf_cli_send        *send;
};

struct oxf_client {
    struct oxf_con_addr   ifaces[OXF_CLIENT_MAX_CON];
    struct oxf_client_con *connections[OXF_CLIENT_MAX_CON];
    struct oxf_client_ops *ops;
    uint16_t n_con;
    uint16_t n_ifaces;
};

struct oxf_client *oxf_udp_client_init (void);
void               oxf_udp_client_exit (struct oxf_client *client);
struct oxf_client *oxf_tcp_client_init (void);
void               oxf_tcp_client_exit (struct oxf_client *client);

int oxf_get_sgl_desc_length (NvmeSGLDesc *desc);

/* HOST FABRICS */

void     oxf_host_exit (void);
int      oxf_host_init (void);
int      oxf_host_add_server_iface (const char *addr, uint16_t port);
uint16_t oxf_host_queue_count (void);
void     oxf_host_destroy_queue (uint16_t qid);
int      oxf_host_create_queue (uint16_t qid);
int      oxf_host_submit_admin (struct nvme_cmd *ncmd, oxf_callback_fn *cb,
                                                                    void *ctx);
int      oxf_host_submit_io (uint16_t qid, struct nvme_cmd *ncmd,
                                struct nvme_sgl_desc *desc, uint16_t sgl_size,
                                oxf_callback_fn *cb, void *ctx);

void                  oxf_host_free_sgl (struct nvme_sgl_desc *desc);
struct nvme_sgl_desc *oxf_host_alloc_sgl (uint8_t **buf_list, uint32_t *buf_sz,
                                                              uint16_t entries);
struct nvme_sgl_desc *oxf_host_alloc_keyed_sgl (uint8_t **buf_list,
                            uint32_t *buf_sz, uint64_t *keys, uint16_t entries);

#endif /* OX_FABRICS_H */
