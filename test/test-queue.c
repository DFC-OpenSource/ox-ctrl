#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <libox.h>
#include <nvme.h>
#include <ox-fabrics.h>

#define NUM_CMD     100000
#define NUM_QUEUES  4 * (OXF_FULL_IFACES + 1)

volatile static uint32_t returned;
volatile static uint32_t completed;
static pthread_spinlock_t spin;

static struct timespec ts_s, ts_e;
static uint64_t start, end, sent, received;

void test_callback (void *ctx, struct nvme_cqe *cqe)
{
    struct nvme_cmd *ncmd = (struct nvme_cmd *) ctx;
    double iops, th;

    if (cqe->status)
        printf ("Command (%d/0x%x) returned. Status -> %d\n",
                                          cqe->cid, ncmd->opcode, cqe->status);

    pthread_spin_lock (&spin);
    if ((cqe->cid == ncmd->cid) && (cqe->status != NVME_HOST_TIMEOUT))
        completed++;

    returned--;
    pthread_spin_unlock (&spin);

    if (!returned) {
        GET_NANOSECONDS(end,ts_e);
        printf ("Time elapsed %lu us. Completed: %d\n",
                                              (end - start) / 1000, completed);

        iops = ( (double)1 * (double)NUM_CMD ) /
                      ( ( (double)end - (double)start ) / (double) 1000000000 );

        th = ( (double)1 * ( (sent + received) / (double)1024 /(double)1024 )) /
                ( ( (double)end - (double)start ) / (double) 1000000000 );

        printf ("IOPS: %.2lf, th: %.2lf MB/s\n", iops, th);
        printf ("Data sent: %lu MB\n", sent / 1024 / 1024);
        printf ("Data rcvd: %lu MB\n", received / 1024 / 1024);
    }
}

int main (void)
{
    int ret, i, cmd_i, q_id;
    struct nvme_cmd *ncmd[NUM_CMD];
    struct nvme_sgl_desc *desc;
    uint8_t buffer[OXF_SQC_MAX_DATA];
    uint8_t  *buf_ptr[1] = {&buffer[0]};
    uint32_t buf_sz[1] = {4096 * 15};

    for (cmd_i = 0; cmd_i < NUM_CMD; cmd_i++) {
        ncmd[cmd_i] = calloc (1, sizeof (struct nvme_cmd));
        if (!ncmd[cmd_i]) {
            printf ("Failed to allocating memory.\n");
            goto FREE;
        }
    }

    if (pthread_spin_init (&spin, 0))
        goto FREE;

    ret = oxf_host_init ();
    if (ret) {
        printf ("Failed to initializing Host Fabrics.\n");
        goto SPIN;
    }

    oxf_host_add_server_iface (OXF_ADDR_1, OXF_PORT_1);
    oxf_host_add_server_iface (OXF_ADDR_2, OXF_PORT_2);

/* We just have 2 cables for now, for the real network setup */
#if OXF_FULL_IFACES
    oxf_host_add_server_iface (OXF_ADDR_3, OXF_PORT_3);
    oxf_host_add_server_iface (OXF_ADDR_4, OXF_PORT_4);
#endif

    for (q_id = 0; q_id < NUM_QUEUES + 1; q_id++) {
        if (oxf_host_create_queue (q_id)) {
            printf ("Failed to creating queue %d.\n", q_id);
            goto EXIT;
        }
    }

    printf ("Connected queues: %d + 1 (admin)\n", NUM_QUEUES);

    returned = NUM_CMD;
    completed = 0;
    sent = 0;
    received = 0;

    for (int i2 = 0; i2 < OXF_SQC_MAX_DATA; i2++)
        buffer[i2] = (uint8_t) (i2 % 256);

    GET_NANOSECONDS(start,ts_s);
    for (i = 0; i < NUM_CMD; i++) {
        if (i % 2 == 0)
            ncmd[i]->opcode = NVME_CMD_WRITE_NULL;
        else
            ncmd[i]->opcode = NVME_CMD_READ_NULL;

        desc = oxf_host_alloc_sgl (buf_ptr, buf_sz, 1);
        if (!desc)
            goto EXIT;

        if (oxf_host_submit_io ((i % NUM_QUEUES) + 1, ncmd[i], desc, 1,
                                                     test_callback, ncmd[i])) {
            printf ("Failed to submitting command %d.\n", i);
            pthread_spin_lock (&spin);
            returned--;
            pthread_spin_unlock (&spin);
        } else {
            if (ncmd[i]->opcode == NVME_CMD_WRITE_NULL) {
                sent += OXF_FAB_CMD_SZ + NVMEF_SGL_SZ + desc->data.length;
                received += OXF_FAB_CQE_SZ;
            } else {
                sent += OXF_FAB_CMD_SZ + NVMEF_SGL_SZ;
                received += OXF_FAB_CQE_SZ + desc->data.length;
            }
        }

        oxf_host_free_sgl (desc);
    }
    
    printf ("Waiting completion...\n");

    while (returned) {
        usleep (100000);
    }

EXIT:
    while (q_id) {
        q_id--;
        oxf_host_destroy_queue (q_id);
    }
    oxf_host_exit ();
SPIN:
    pthread_spin_destroy (&spin);
FREE:
    while (cmd_i) {
        cmd_i--;
        free (ncmd[cmd_i]);
    }
    return -1;
}