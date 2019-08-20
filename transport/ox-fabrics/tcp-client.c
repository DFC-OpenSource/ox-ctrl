/* OX: Open-Channel NVM Express SSD Controller
 *
 *  - OX NVMe over TCP (client side) 
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

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <ox-fabrics.h>

static uint16_t oxf_tcp_client_process_msg (struct oxf_client_con *con,
                uint8_t *buffer, uint8_t *broken, uint16_t *brkb,
                int msg_bytes)
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
        con->recv_fn (msg_sz, (void *) broken);
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
        con->recv_fn (msg_sz, (void *) &buffer[offset]);
        offset += msg_sz;
    }

    return brk_bytes;
}

static void *oxf_tcp_client_recv (void *arg)
{
    ssize_t msg_bytes;
    uint16_t brk_bytes = 0;
    uint8_t buffer[OXF_MAX_DGRAM + 1];
    uint8_t broken[OXF_MAX_DGRAM + 1];
    struct oxf_client_con *con = (struct oxf_client_con *) arg;

    while (con->running) {
        msg_bytes = recv(con->sock_fd , buffer, OXF_MAX_DGRAM, MSG_DONTWAIT);

        if (msg_bytes <= 0)
            continue;

        brk_bytes = oxf_tcp_client_process_msg (con, buffer, broken,
                                                        &brk_bytes, msg_bytes);
    }

    return NULL;
}

static struct oxf_client_con *oxf_tcp_client_connect (struct oxf_client *client,
       uint16_t cid, const char *addr, uint16_t port, oxf_rcv_reply_fn *recv_fn)
{
    struct oxf_client_con *con;
    unsigned int len;
    struct timeval tv;

    if (cid >= OXF_SERVER_MAX_CON) {
        printf ("[ox-fabrics: Invalid connection ID: %d]\n", cid);
        return NULL;
    }

    if (client->connections[cid]) {
        printf ("[ox-fabrics: Connection already established: %d]\n", cid);
        return NULL;
    }

    con = calloc (1, sizeof (struct oxf_client_con));
    if (!con)
	return NULL;

    con->cid = cid;
    con->client = client;
    con->recv_fn = recv_fn;

    if ( (con->sock_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0 ) {
        printf ("[ox-fabrics: Socket creation failure]\n");
        free (con);
        return NULL;
    }

    len = sizeof (struct sockaddr);
    con->addr.sin_family = AF_INET;
    inet_aton (addr, (struct in_addr *) &con->addr.sin_addr.s_addr);
    con->addr.sin_port = htons(port);

    tv.tv_sec = 0;
    tv.tv_usec = OXF_RCV_TO;

    if (setsockopt(con->sock_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0){
        printf ("[ox-fabrics: Socket timeout failure.]\n");
        goto NOT_CONNECTED;
    }

    if ( connect(con->sock_fd, (const struct sockaddr *) &con->addr , len) < 0){
        printf ("[ox-fabrics: Socket connection failure.]\n");
        goto NOT_CONNECTED;
    }

    con->running = 1;
    if (pthread_create(&con->recv_th, NULL, oxf_tcp_client_recv, (void *) con)){
        printf ("[ox-fabrics: Receive reply thread not started.]\n");
        goto NOT_CONNECTED;
    }

    client->connections[cid] = con;
    client->n_con++;

    return con;

NOT_CONNECTED:
    con->running = 0;
    shutdown (con->sock_fd, 0);
    close (con->sock_fd);
    free (con);
    return NULL;
}

static int oxf_tcp_client_send (struct oxf_client_con *con, uint32_t size,
                                                                const void *buf)
{
    uint32_t ret;

    ret = send(con->sock_fd, buf, size, 0);
    if (ret != size)
        return -1;

    return 0;
}

static void oxf_tcp_client_disconnect (struct oxf_client_con *con)
{
    if (con) {
        con->running = 0;
        pthread_join (con->recv_th, NULL);
        shutdown (con->sock_fd, 0);
        close (con->sock_fd);
        con->client->connections[con->cid] = NULL;
        con->client->n_con--;
        free (con);
    }
}

void oxf_tcp_client_exit (struct oxf_client *client)
{
    free (client);
}

struct oxf_client_ops oxf_tcp_cli_ops = {
    .connect    = oxf_tcp_client_connect,
    .disconnect = oxf_tcp_client_disconnect,
    .send       = oxf_tcp_client_send
};

struct oxf_client *oxf_tcp_client_init (void)
{
    struct oxf_client *client;

    client = calloc (1, sizeof (struct oxf_client));
    if (!client)
	return NULL;

    client->ops = &oxf_tcp_cli_ops;

    return client;
}