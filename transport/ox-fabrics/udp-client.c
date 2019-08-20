/* OX: Open-Channel NVM Express SSD Controller
 *
 *  - OX NVMe over UDP (client side) 
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

static void *oxf_udp_client_recv (void *arg)
{
    int n;
    unsigned int len;
    uint8_t buf[OXF_MAX_DGRAM + 1];
    struct oxf_client_con *con = (struct oxf_client_con *) arg;

    len = sizeof (struct sockaddr);
    while (con->running) {
        n = recvfrom(con->sock_fd, (char *) buf, OXF_MAX_DGRAM,  
                            MSG_WAITALL, ( struct sockaddr *) &con->addr, &len);
        if (n > 0)
            con->recv_fn (n, (void *) buf);
    }

    return NULL;
}

static struct oxf_client_con *oxf_udp_client_connect (struct oxf_client *client,
       uint16_t cid, const char *addr, uint16_t port, oxf_rcv_reply_fn *recv_fn)
{
    struct oxf_client_con *con;
    uint8_t connect[2], ack[2];
    unsigned int len;
    struct timeval tv;
    int n;

    if (cid >= OXF_SERVER_MAX_CON) {
        printf ("[ox-fabrics: Invalid connection ID: %d]", cid);
        return NULL;
    }

    if (client->connections[cid]) {
        printf ("[ox-fabrics: Connection already established: %d]", cid);
        return NULL;
    }

    con = calloc (1, sizeof (struct oxf_client_con));
    if (!con)
	return NULL;

    /* Change port for different connections */
    port = port + cid;

    con->cid = cid;
    con->client = client;
    con->recv_fn = recv_fn;

    if ( (con->sock_fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0 ) { 
        free (con);
        return NULL;
    }

    con->addr.sin_family = AF_INET;
    inet_aton (addr, (struct in_addr *) &con->addr.sin_addr.s_addr);
    con->addr.sin_port = htons(port);

    /* Check connectivity */
    connect[0] = OXF_CON_BYTE;
    len = sizeof (struct sockaddr);
    tv.tv_sec = 0;
    tv.tv_usec = OXF_RCV_TO;

    if (setsockopt(con->sock_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0){
        printf ("[ox-fabrics: Socket timeout failure.]\n");
        goto NOT_CONNECTED;
    }

    sendto(con->sock_fd, (void *) connect, 1, MSG_CONFIRM,
                                   (const struct sockaddr *) &con->addr, len);

    n = recvfrom(con->sock_fd, (char *) ack, OXF_MAX_DGRAM,  
                            MSG_WAITALL, ( struct sockaddr *) &con->addr, &len);
    if (n != 1) {
        printf ("[ox-fabrics: Server does not respond.]\n");
        goto NOT_CONNECTED;
    }

    if (ack[0] != OXF_ACK_BYTE) {
        printf ("[ox-fabrics: Server responded, but byte is incorrect: %x]\n",
                                                                        ack[0]);
        goto NOT_CONNECTED;
    }

    con->running = 1;
    if (pthread_create(&con->recv_th, NULL, oxf_udp_client_recv, (void *) con)){
        printf ("[ox-fabrics: Receive reply thread not started.]");
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

static int oxf_udp_client_send (struct oxf_client_con *con, uint32_t size,
                                                                const void *buf)
{
    unsigned int len;
    uint32_t ret;

    len = sizeof (struct sockaddr);

    ret = sendto(con->sock_fd, buf, size, MSG_CONFIRM,
                        (const struct sockaddr *) &con->addr, len);
    if (ret != size)
        return -1;

    return 0;
}

static void oxf_udp_client_disconnect (struct oxf_client_con *con)
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

void oxf_udp_client_exit (struct oxf_client *client)
{
    free (client);
}

struct oxf_client_ops oxf_udp_cli_ops = {
    .connect    = oxf_udp_client_connect,
    .disconnect = oxf_udp_client_disconnect,
    .send       = oxf_udp_client_send
};

struct oxf_client *oxf_udp_client_init (void)
{
    struct oxf_client *client;

    client = calloc (1, sizeof (struct oxf_client));
    if (!client)
	return NULL;

    client->ops = &oxf_udp_cli_ops;

    return client;
}