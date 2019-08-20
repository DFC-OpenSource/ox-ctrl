/* OX: Open-Channel NVM Express SSD Controller
 *
 *  - OX NVMe over UDP (server side) 
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
#include <libox.h>

static struct oxf_server_con *oxf_udp_server_bind (struct oxf_server *server,
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

    /* Change the port for different connections */
    port = port + cid;

    con = malloc (sizeof (struct oxf_server_con));
    if (!con)
	return NULL;

    con->cid = cid;
    con->server = server;
    con->running = 0;

    if ( (con->sock_fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0 ) { 
        free (con);
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

    server->connections[cid] = con;
    server->n_con++;

    memcpy (con->haddr.addr, addr, 15);
    con->haddr.addr[15] = '\0';
    con->haddr.port = port;

    return con;

ERR:
    shutdown (con->sock_fd, 2);
    close (con->sock_fd);
    free (con);
    return NULL;
}

static void oxf_udp_server_unbind (struct oxf_server_con *con)
{
    if (con) {
        shutdown (con->sock_fd, 0);
        close (con->sock_fd);
        con->server->connections[con->cid] = NULL;
        con->server->n_con--;
        free (con);
    }
}

static void *oxf_udp_server_con_th (void *arg)
{
    struct oxf_server_con *con = (struct oxf_server_con *) arg;
    struct sockaddr_in client;
    uint8_t buffer[OXF_MAX_DGRAM + 1];
    uint8_t ack[2];
    unsigned int len;
    int n;

    ack[0] = OXF_ACK_BYTE;
    len = sizeof (struct sockaddr);
    while (con->running) {
	n = recvfrom(con->sock_fd, (char *) buffer, OXF_MAX_DGRAM,
                            MSG_WAITALL, ( struct sockaddr *) &client, &len);
        buffer[n] = '\0';

        if (n < 0)
            continue;

        if (n == 1 && (buffer[0] == OXF_CON_BYTE) )
            goto ACK;
        
        con->rcv_fn (n, (void *) buffer, (void *) &client);
        continue;

ACK:
        /* Send a MSG_CONFIRM to the client to confirm connection */        
        if (sendto(con->sock_fd, (char *) ack, 1, MSG_CONFIRM,
                            (const struct sockaddr *) &client, len) != 1)
            log_err ("[ox-fabrics: Connect ACK hasn't been sent.]");
            
    }

    log_err ("[ox-fabrics: Connection %d is closed.]", con->cid);

    return NULL;
}

static int oxf_udp_server_reply(struct oxf_server_con *con, const void *buf,
                                                 uint32_t size, void *recv_cli)
{
    struct sockaddr *client = (struct sockaddr *) recv_cli;
    unsigned int len;
    int ret;

    len = sizeof (struct sockaddr);
    ret = sendto(con->sock_fd, buf, size, MSG_CONFIRM,
                                            (struct sockaddr *) client, len);
    if (ret != size) {
        log_err ("[ox-fabrics: Completion reply hasn't been sent. %d]", ret);
        return -1;
    }

    return 0;
}

static int oxf_udp_server_con_start (struct oxf_server_con *con, oxf_rcv_fn *fn)
{
    con->running = 1;
    con->rcv_fn = fn;

    if (pthread_create (&con->tid, NULL, oxf_udp_server_con_th, con)) {
	log_err ("[ox-fabrics: Connection not started.]");
	con->running = 0;
	return -1;
    }

    return 0;
}

static void oxf_udp_server_con_stop (struct oxf_server_con *con)
{
    if (con && con->running)
	con->running = 0;
    else
        return;

    pthread_join (con->tid, NULL);
}

void oxf_udp_server_exit (struct oxf_server *server)
{
    free (server);
}

struct oxf_server_ops oxf_udp_srv_ops = {
    .bind    = oxf_udp_server_bind,
    .unbind  = oxf_udp_server_unbind,
    .start   = oxf_udp_server_con_start,
    .stop    = oxf_udp_server_con_stop,
    .reply   = oxf_udp_server_reply
};

struct oxf_server *oxf_udp_server_init (void)
{
    struct oxf_server *server;

    server = calloc (1, sizeof (struct oxf_server));
    if (!server)
	return NULL;

    server->ops = &oxf_udp_srv_ops;

    log_info ("[ox-fabrics: Protocol -> UDP\n");

    return server;
}