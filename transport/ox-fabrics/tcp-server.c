/* OX: Open-Channel NVM Express SSD Controller
 *
 *  - OX NVMe over TCP (server side) 
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

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sched.h>
#include <errno.h>
#include <ox-fabrics.h>
#include <libox.h>

#define OXF_TCP_DEBUG   0

/* Last connection ID that has received a 'connect' command */
uint16_t pending_conn;

static struct oxf_server_con *oxf_tcp_server_bind (struct oxf_server *server,
                                uint16_t cid, const char *addr, uint16_t port)
{
    struct oxf_server_con *con;
    struct timeval tv;

    if (cid > OXF_SERVER_MAX_CON) {
        log_err ("[ox-fabrics (bind): Invalid connection ID: %d]", cid);
        return NULL;
    }

    if (server->connections[cid]) {
        log_err ("[ox-fabrics (bind): Connection already established: %d]", cid);
        return NULL;
    }

    con = ox_malloc (sizeof (struct oxf_server_con), OX_MEM_TCP_SERVER);
    if (!con)
	return NULL;

    con->cid = cid;
    con->server = server;
    con->running = 0;
    memset (con->active_cli, 0x0, OXF_SERVER_MAX_CON * sizeof (int));

    if ( (con->sock_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0 ) {
        log_err ("[ox-fabrics (bind): Socket creation failure. %d]", con->sock_fd);
        ox_free (con, OX_MEM_TCP_SERVER);
        return NULL;
    }

    con->addr.sin_family = AF_INET;
    inet_aton (addr, (struct in_addr *) &con->addr.sin_addr.s_addr);
    con->addr.sin_port = htons(port);

    if ( bind(con->sock_fd, (const struct sockaddr *) &con->addr,
                					sizeof(con->addr)) < 0 ) 
    {
        log_err ("[ox-fabrics (bind): Socket bind failure.]");
        goto ERR;
    }

    /* Set socket timeout */
    tv.tv_sec = 0;
    tv.tv_usec = OXF_RCV_TO;

    if (setsockopt(con->sock_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0){
        log_err ("[ox-fabrics (bind): Socket timeout failure.]");
        goto ERR;
    }

    /* Put the socket in listen mode to accepting connections */
    if (listen (con->sock_fd, 16)) {
        log_err ("[ox-fabrics (bind): Socket listen failure.]");
        goto ERR;
    }

    server->connections[cid] = con;
    server->n_con++;

    memcpy (con->haddr.addr, addr, 15);
    con->haddr.addr[15] = '\0';
    con->haddr.port = port;

    return con;

ERR:
    shutdown (con->sock_fd, 2);
    close (con->sock_fd);
    ox_free (con, OX_MEM_TCP_SERVER);
    return NULL;
}

static void oxf_tcp_server_unbind (struct oxf_server_con *con)
{
    if (con) {
        shutdown (con->sock_fd, 0);
        close (con->sock_fd);
        con->server->connections[con->cid] = NULL;
        con->server->n_con--;
        ox_free (con, OX_MEM_TCP_SERVER);
    }
}

static uint16_t oxf_tcp_server_process_msg (struct oxf_server_con *con,
                uint8_t *buffer, uint8_t *broken, uint16_t *brkb,
                uint16_t conn_id, int msg_bytes)
{
    uint16_t offset = 0, fix = 0, msg_sz, brk_bytes = *brkb;

    if (brk_bytes) {

        if (brk_bytes < 3) {

            if (msg_bytes + brk_bytes < 3) {
                memcpy (&broken[brk_bytes], buffer, msg_bytes);
                brk_bytes += msg_bytes;
                return brk_bytes;
            }

            memcpy (&broken[brk_bytes], buffer, 3 - brk_bytes);
            offset = fix = 3 - brk_bytes;
            msg_bytes -= 3 - brk_bytes;
            brk_bytes = 3;
            if (!msg_bytes)
                return brk_bytes;
        }

        msg_sz = ((struct oxf_capsule_sq *) broken)->size;

        if (brk_bytes + msg_bytes < msg_sz) {
            memcpy (&broken[brk_bytes], &buffer[offset], msg_bytes);
            brk_bytes += msg_bytes;
            return brk_bytes;
        }

        memcpy (&broken[brk_bytes], &buffer[offset], msg_sz - brk_bytes);
        con->rcv_fn (msg_sz, (void *) broken,
                                            (void *) &con->active_cli[conn_id]);
        offset += msg_sz - brk_bytes;
        brk_bytes = 0;
    }

    msg_bytes += fix;
    while (offset < msg_bytes) {
        if ( (msg_bytes - offset < 3) ||
            (msg_bytes - offset <
                         ((struct oxf_capsule_sq *) &buffer[offset])->size) ) {
            memcpy (broken, &buffer[offset], msg_bytes - offset);
            brk_bytes = msg_bytes - offset;
            offset += msg_bytes - offset;
            continue;
        }

        msg_sz = ((struct oxf_capsule_sq *) &buffer[offset])->size;
        con->rcv_fn (msg_sz, (void *) &buffer[offset],
                                           (void *) &con->active_cli[conn_id]);
        offset += msg_sz;
    }

    return brk_bytes;
}

static void *oxf_tcp_server_con_th (void *arg)
{
    struct oxf_server_con *con = (struct oxf_server_con *) arg;
    uint16_t brk_bytes = 0;
    uint16_t conn_id = pending_conn;
    uint8_t buffer[OXF_MAX_DGRAM + 1];
    uint8_t broken[OXF_MAX_DGRAM + 1];
    int msg_bytes;

    /* Set thread affinity, if enabled */
#if OX_TH_AFFINITY
    cpu_set_t cpuset;
    pthread_t current_thread;

    CPU_ZERO(&cpuset);
    CPU_SET(1, &cpuset);
    CPU_SET(2, &cpuset);
    CPU_SET(3, &cpuset);
    CPU_SET(7, &cpuset);

    current_thread = pthread_self();
    pthread_setaffinity_np(current_thread, sizeof(cpu_set_t), &cpuset);

    log_info (" [tcp: Thread affinity is set for connection %d\n", con->cid);
#endif /* OX_TH_AFFINITY */

    pending_conn = 0;
    log_info ("[ox-fabrics: Connection %d is started -> client %d\n",
                                            conn_id, con->active_cli[conn_id]);

    while (con->active_cli[conn_id] > 0) {

        msg_bytes = recv(con->active_cli[conn_id] - 1,
                                            buffer, OXF_MAX_DGRAM, MSG_DONTWAIT);
        buffer[msg_bytes] = '\0';

        /* Timeout */
        if (msg_bytes < 0)
            continue;

        /* Client disconnected */
        if (msg_bytes == 0)
            break;

        if (OXF_TCP_DEBUG)
            printf ("tcp: Received message: %d bytes\n", msg_bytes);

        brk_bytes = oxf_tcp_server_process_msg (con, buffer, broken,
                                               &brk_bytes, conn_id, msg_bytes);
    }

    close (con->active_cli[conn_id] - 1);
    con->active_cli[conn_id] = 0;
    log_info ("[ox-fabrics: Connection %d is closed.]", conn_id);

    return NULL;
}

static void *oxf_tcp_server_accept_th (void *arg)
{
    struct oxf_server_con *con = (struct oxf_server_con *) arg;
    struct sockaddr_in client;
    int client_sock;
    unsigned int len;

    len = sizeof (struct sockaddr);

    log_info ("[ox-fabrics: Accepting connections -> %s:%d\n", con->haddr.addr,
                                                               con->haddr.port);

    while (con->running) {

        //accept connection from an incoming client
        client_sock = accept(con->sock_fd, (struct sockaddr *) &client, &len);

        if (client_sock < 0)
            continue;

        if (con->active_cli[pending_conn]) {
            log_info ("[ox-fabrics: Client %d is taking connection %d.]",
                                                    client_sock, pending_conn);
            con->active_cli[pending_conn] = 0;
            pthread_join (con->cli_tid[pending_conn], NULL);
        }

        con->active_cli[pending_conn] = client_sock + 1;

        if (pthread_create (&con->cli_tid[pending_conn], NULL,
                                        oxf_tcp_server_con_th, (void *) arg)) {
            pending_conn = 0;
            log_err ("[ox-fabrics: Client thread not started: %d]",
                                                                 pending_conn);
        }

        while (pending_conn)
            usleep (1000);
    }

    return NULL;
}

static int oxf_tcp_server_reply(struct oxf_server_con *con, const void *buf,
                                                 uint32_t size, void *recv_cli)
{
    int *client = (int *) recv_cli;
    int ret;

    ret = send (*client - 1, buf, size, 0);

    if (OXF_TCP_DEBUG)
        printf ("tcp: Message replied: %d bytes\n", size);

    if (ret != size) {
        log_err ("[ox-fabrics: Completion reply hasn't been sent. %d]", ret);
        return -1;
    }

    return 0;
}

static int oxf_tcp_server_con_start (struct oxf_server_con *con, oxf_rcv_fn *fn)
{
    if (con->running)
        return 0;

    con->running = 1;
    con->rcv_fn = fn;

    if (pthread_create (&con->tid, NULL, oxf_tcp_server_accept_th, con)) {
	log_err ("[ox-fabrics: Connection not started.]");
	con->running = 0;
	return -1;
    }

    return 0;
}

static void oxf_tcp_server_con_stop (struct oxf_server_con *con)
{
    uint32_t cli_id;

    if (con && con->running)
	con->running = 0;
    else
        return;

    for (cli_id = 0; cli_id < OXF_SERVER_MAX_CON; cli_id++) {
        if (con->active_cli[cli_id]) {
            con->active_cli[cli_id] = 0;
            pthread_join (con->cli_tid[cli_id], NULL);
        }
    }
    pthread_join (con->tid, NULL);
}

void oxf_tcp_server_exit (struct oxf_server *server)
{
    uint32_t con_i;

    for (con_i = 0; con_i < OXF_SERVER_MAX_CON; con_i++)
        oxf_tcp_server_con_stop (server->connections[con_i]);

    ox_free (server, OX_MEM_TCP_SERVER);
}

struct oxf_server_ops oxf_tcp_srv_ops = {
    .bind    = oxf_tcp_server_bind,
    .unbind  = oxf_tcp_server_unbind,
    .start   = oxf_tcp_server_con_start,
    .stop    = oxf_tcp_server_con_stop,
    .reply   = oxf_tcp_server_reply
};

struct oxf_server *oxf_tcp_server_init (void)
{
    struct oxf_server *server;

    if (!ox_mem_create_type ("TCP_SERVER", OX_MEM_TCP_SERVER))
        return NULL;

    server = ox_calloc (1, sizeof (struct oxf_server), OX_MEM_TCP_SERVER);
    if (!server)
	return NULL;

    server->ops = &oxf_tcp_srv_ops;
    pending_conn = 0;

    log_info ("[ox-fabrics: Protocol -> TCP\n");

    return server;
}
