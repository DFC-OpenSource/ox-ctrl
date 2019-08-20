/*  OX: Open-Channel NVM Express SSD Controller
 *
 *  - Multi-Queue Support for Parallel I/O - Output data and statistics
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
 */

#include <sys/queue.h>
#include <pthread.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <ox-mq.h>
#include <libox.h>

#define OXMQ_OUTPUT_BUF_SZ  8192 

struct oxmq_output *ox_mq_output_init (uint64_t id, const char *name,
                                                                uint32_t nodes)
{
    FILE *fp;
    char filename[80];
    struct stat st = {0};
    struct oxmq_output *output;
    uint32_t node_i;

    if (strlen(name) > 40)
        return NULL;

    output = ox_malloc (sizeof (struct oxmq_output), OX_MEM_OX_MQ);
    if (!output)
        return NULL;

    output->id = id;
    output->nodes = nodes;
    strcpy (output->name, name);

    if (stat("output", &st) == -1)
        mkdir("output", S_IRWXO);

    sprintf (filename, "output/%lu_%s.csv", id, name);
    fp = fopen(filename, "a");
    if (!fp)
        goto FREE_OUT;

    fprintf (fp, "node_sequence;node_id;lba;channel;lun;block;page;"
        "plane;sector;start;end;latency;type;is_failed;read_memcmp;bytes\n");

    fclose(fp);

    output->node_seq = ox_calloc (sizeof(uint64_t), nodes, OX_MEM_OX_MQ);
    if (!output->node_seq)
        goto FREE_OUT;

    output->queues = ox_malloc (sizeof (struct oxmq_output_tq) * nodes,
                                                                OX_MEM_OX_MQ);
    if (!output->queues)
        goto FREE_NODE;

    for (node_i = 0; node_i < nodes; node_i++) {
        output->queues[node_i].rows = ox_malloc (sizeof (struct oxmq_output_row) *
                                            OXMQ_OUTPUT_BUF_SZ, OX_MEM_OX_MQ);
        if (!output->queues[node_i].rows)
            goto FREE_QUEUE;
        output->queues[node_i].row_off = 0;
        TAILQ_INIT (&output->queues[node_i].out_head);
    }

    if (pthread_mutex_init (&output->file_mutex, 0))
        goto FREE_QUEUE;

    output->sequence = 0;

    return output;

FREE_QUEUE:
    while (node_i) {
        node_i--;
        ox_free (output->queues[node_i].rows, OX_MEM_OX_MQ);
    }
    ox_free (output->queues, OX_MEM_OX_MQ);
FREE_NODE:
    ox_free (output->node_seq, OX_MEM_OX_MQ);
FREE_OUT:
    ox_free (output, OX_MEM_OX_MQ);
    return NULL;
}

void ox_mq_output_exit (struct oxmq_output *output)
{
    uint32_t node_i;

    pthread_mutex_destroy (&output->file_mutex);

    for (node_i = 0; node_i < output->nodes; node_i++)
        ox_free (output->queues[node_i].rows, OX_MEM_OX_MQ);

    ox_free (output->queues, OX_MEM_OX_MQ);
    ox_free (output->node_seq, OX_MEM_OX_MQ);
    ox_free (output, OX_MEM_OX_MQ);
}

static int ox_mq_output_flush_node (struct oxmq_output *output, int node_i)
{
    struct oxmq_output_row *row;
    char tstart[21], tend[21];
    uint32_t row_i;
    FILE *fp;
    char filename[80];

    sprintf (filename, "output/%lu_%s.csv", output->id, output->name);
    fp = fopen(filename, "a");
    if (!fp) {
        printf (" [ox-mq: ERROR. File not opened.]\n");
        return -1;
    }

    for (row_i = 0; row_i < output->queues[node_i].row_off; row_i++) {

        row = &output->queues[node_i].rows[row_i];
        if (!row) {
            fclose(fp);
            return -1;
        }

        row->ulat = (row->tend - row->tstart) / 1000;
        sprintf (tstart, "%lu", row->tstart);
        sprintf (tend, "%lu", row->tend);
        memmove (tstart, tstart+4, 17);
        memmove (tend, tend+4, 17);

        if(fprintf (fp,
                "%lu;"
                "%d;"
                "%lu;"
                "%d;"
                "%d;"
                "%d;"
                "%d;"
                "%d;"
                "%d;"
                "%s;"
                "%s;"
                "%lu;"
                "%c;"
                "%d;"
                "%d;"
                "%d\n",
                row->node_seq,
                row->node_id,
                row->lba,
                row->ch,
                row->lun,
                row->blk,
                row->pg,
                row->pl,
                row->sec,
                tstart,
                tend,
                row->ulat,
                row->type,
                row->failed,
                row->datacmp,
                row->size) < 0) {
            printf (" [ox-mq: ERROR. Not possible flushing results.]\n");
            fclose(fp);
            return -1;
        }
    }
    
    fclose(fp);
    return 0;
}

void ox_mq_output_flush (struct oxmq_output *output)
{
    uint32_t node_i;

    pthread_mutex_lock (&output->file_mutex);

    for (node_i = 0; node_i < output->nodes; node_i++)
        ox_mq_output_flush_node (output, node_i);

    pthread_mutex_unlock (&output->file_mutex);
}

struct oxmq_output_row *ox_mq_output_new (struct oxmq_output *output,
                                                                    int node_id)
{
    struct oxmq_output_row *row;

    if (output->queues[node_id].row_off >= OXMQ_OUTPUT_BUF_SZ) {
        pthread_mutex_lock (&output->file_mutex);
        if (!output->queues[node_id].row_off) {
            pthread_mutex_unlock (&output->file_mutex);
            goto GET;
        }

        ox_mq_output_flush_node (output, node_id);
        memset (output->queues[node_id].rows, 0x0,
                        sizeof (struct oxmq_output_row) * OXMQ_OUTPUT_BUF_SZ);
        output->queues[node_id].row_off = 0;

        pthread_mutex_unlock (&output->file_mutex);
    }

GET:
    row = &output->queues[node_id].rows[output->queues[node_id].row_off];
    output->queues[node_id].row_off++;

    row->node_id = node_id;
    row->node_seq = output->node_seq[node_id];
    output->node_seq[node_id]++;

    return row;
}