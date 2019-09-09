#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <nvme-host.h>

#define NVMEH_NUM_QUEUES    4 * (OXF_FULL_IFACES + 1)
#define NVMEH_BUF_SIZE      1024 * 1024 * 8 /* 8 MB */

#define NVMEH_BLKSZ 4096

static volatile uint8_t done;

/* This is an example context that identifies the completion */
struct nvme_test_context {
    uint64_t    slba;
    uint64_t    nlb;
    uint8_t     is_write;
};

void nvme_test_callback (void *ctx, uint16_t status)
{
    struct nvme_test_context *my_ctx = (struct nvme_test_context *) ctx;

    printf ("Is write: %d, LBA %lu-%lu, size: %lu KB. Status -> %x\n",
                my_ctx->is_write, my_ctx->slba, my_ctx->slba + my_ctx->nlb,
                NVMEH_BLK_SZ * my_ctx->nlb / 1024, status);

    done++;
}

void nvme_test_read (void)
{
    int ret;
    uint8_t *read_buffer;
    uint8_t *write_buffer;
    struct nvme_test_context ctx[2];
    uint64_t slba;

    /* An example of write buffer */
    write_buffer = malloc (NVMEH_BUF_SIZE);
    if (!write_buffer) {
        printf ("Memory allocation error.\n");
        return;
    }
    memset (write_buffer, 0xca, NVMEH_BUF_SIZE);
    
    /* An example of read buffer */
    read_buffer = malloc (NVMEH_BUF_SIZE);
    if (!read_buffer) {
        free (write_buffer);
        printf ("Memory allocation error.\n");
        return;
    }

    /* Example of SLBA */
    slba = 100;

    /* Sets the example context for write */
    ctx[0].slba = slba;
    ctx[0].is_write = 1;
    ctx[0].nlb = NVMEH_BUF_SIZE / NVMEH_BLK_SZ;

    /* Submit the write command and define the callback function */
    ret = nvmeh_write (write_buffer, NVMEH_BUF_SIZE, slba,
                                                nvme_test_callback, &ctx[0]);
    if (ret) {
        printf ("Write has failed.\n");
        done++;
    }

    /* Wait until the write returns asynchronously */
    while (done < 1) {
        usleep (100);
    }

    /* Sets the example for read */
    ctx[1].slba = slba;
    ctx[1].is_write = 0;
    ctx[1].nlb = NVMEH_BUF_SIZE / NVMEH_BLK_SZ;

    /* Submit the read command and define the callback function */
    ret = nvmeh_read (read_buffer, NVMEH_BUF_SIZE, slba,
                                                nvme_test_callback, &ctx[1]);
    if (ret) {
        printf ("Read has failed.\n");
        done++;
    }

    /* Wait until the read returns asynchronously */
    while (done < 2) {
        usleep (100);
    }

    if (memcmp (write_buffer, read_buffer, NVMEH_BUF_SIZE))
        printf ("Data is NOT equal.\n");
    else
        printf ("Data is equal.\n");

    free (read_buffer);
    free (write_buffer);
}

int main (void)
{
    int ret, q_id;

    ret = nvmeh_init ();
    if (ret) {
        printf ("Failed to initializing NVMe Host.\n");
        return -1;
    }

    nvme_host_add_server_iface (OXF_ADDR_1, OXF_PORT_1);
    nvme_host_add_server_iface (OXF_ADDR_2, OXF_PORT_2);

/* We just have 2 cables for now, for the real network setup */
#if OXF_FULL_IFACES
    nvme_host_add_server_iface (OXF_ADDR_3, OXF_PORT_3);
    nvme_host_add_server_iface (OXF_ADDR_4, OXF_PORT_4);
#endif

    /* Create the NVMe queues. One additional queue for the admin queue */
    for (q_id = 0; q_id < NVMEH_NUM_QUEUES + 1; q_id++) {
        if (nvme_host_create_queue (q_id)) {
            printf ("Failed to creating queue %d.\n", q_id);
            goto EXIT;
        }
    }

    done = 0;

    /* Read */
    nvme_test_read ();

    /* Closes the application */
EXIT:
    while (q_id) {
        q_id--;
        nvme_host_destroy_queue (q_id);
    }
    nvmeh_exit ();

    return 0;
}
