#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include <libox.h>
#include <nvme-host.h>

/* 1 Admin queue + 4 I/O queues */
#define NVME_TEST_NUM_QUEUES    (4 + 1)

/* Write buffer: 128 MB */
#define NVME_TEST_BUF_SIZE      (1024 * 1024 * 128)
#define NVME_TEST_LBA_BUF       (NVME_TEST_BUF_SIZE / NVMEH_BLK_SZ)

/* I/O write size */
#define NVME_TEST_IO_SZ         (1024 * 32) /* 32 KB */
#define NVME_TEST_LBA_IO        (NVME_TEST_IO_SZ / NVMEH_BLK_SZ)

/* Number of I/Os */
#define NVME_TEST_IOS           1000

/* Offsets containing the LBA for read check */
#define CHECK_OFFSET1           16
#define CHECK_OFFSET2           1852
#define CHECK_OFFSET3           3740

/* Write buffer */
static uint8_t            *write_buffer;

/* Used to keep track of completed LBAs */
static volatile uint64_t   done;
static pthread_spinlock_t  done_spin;

/* Time and statistics variables */
static struct timespec ts_s, ts_e, ts_c;
static uint64_t start, end, cur, firstlba, lastlba, iocount, err;

/* This is an example context that identifies the completion */
struct nvme_test_context {
    uint32_t    id;
    uint64_t    slba;
    uint64_t    nlb;
};
struct nvme_test_context *ctx;

/* Prints statistics at runtime */
static void nvme_test_print_runtime (void)
{
    double iops, data, mb, sec, divb = 1024, divt = 1000, total_lbas;
    uint32_t perc;

    total_lbas = lastlba - firstlba;
    perc = (done * 100) / total_lbas;
    if (perc > 100)
        perc = 100;
    data = (double) NVMEH_BLK_SZ * (double) done;
    mb = data / divb / divb;
    GET_NANOSECONDS(cur,ts_c);
    sec = ((double)cur - (double)start) / divt / divt / divt;
    iops = (done / NVME_TEST_LBA_IO) / sec;
    printf ("\r %d %% - Time %.2lf ms, written: %.2lf MB, thput: %.2lf MB/s, "
            "IOPS: %.1f", perc, sec * divt, mb, mb / sec, iops);
}

/* Callback funcion */
static void nvme_test_callback (void *ctx, uint16_t status)
{
    struct nvme_test_context *my_ctx = (struct nvme_test_context *) ctx;

    pthread_spin_lock (&done_spin);
    done += my_ctx->nlb;
    if (status)
        err++;
    pthread_spin_unlock (&done_spin);

    if (done % NVME_TEST_LBA_IO * 400 == 0)
        nvme_test_print_runtime ();
          
    if (done == lastlba - firstlba + 1) {
        GET_NANOSECONDS(end,ts_e);
        done++;
    }
}

/* Prints statistics at the end */
static void nvme_test_print (void)
{
    double iops, data, mb, sec, divb = 1024, divt = 1000;
    uint64_t time;

    data = (double) NVMEH_BLK_SZ * (double) done - 1;
    mb = data / divb / divb;
    time = end - start;
    sec = (double) time / divt / divt / divt;
    iops = (done - 1) / NVME_TEST_LBA_IO / sec;
    printf ("\n\n Time elapsed  : %.2lf ms\n", (double) time / divt / divt);
    printf (    " Written LBAs  : %lu (%lu to %lu) \n",
                                    lastlba - firstlba + 1, firstlba, lastlba);
    printf (    " Written data  : %.2lf MB\n", mb);
    printf (    " Throughput    : %.2lf MB/s\n", mb / sec);
    printf (    " IOPS          : %.1f\n", iops);
    printf (    " Block size    : %d KB\n", NVMEH_BLK_SZ / 1024);
    printf (    " I/O size      : %d KB\n", NVME_TEST_IO_SZ / 1024);
    printf (    " Issued I/Os   : %lu\n", iocount);
    printf (    " Failed I/Os   : %lu\n\n", err);
}

/* Prepares the write buffer and spinlocks */
static int nvme_test_prepare (void)
{
    uint32_t lba = 1, off = 0;
    uint8_t *buf;

    ctx = malloc (iocount * sizeof (struct nvme_test_context));
    if (!ctx) {
        printf ("Memory allocation error.\n");
        return -1;
    }

    write_buffer = malloc (NVME_TEST_BUF_SIZE + NVME_TEST_IO_SZ);
    if (!write_buffer) {
        printf ("Memory allocation error.\n");
        free (ctx);
        return -1;
    }

    while (off < NVME_TEST_BUF_SIZE + NVME_TEST_IO_SZ - 1) {
        buf = &write_buffer[off];
        memset (buf, (uint8_t) lba + 7, NVMEH_BLK_SZ);
        *((uint32_t *) &buf[CHECK_OFFSET1]) = lba;
        *((uint32_t *) &buf[CHECK_OFFSET2]) = lba;
        *((uint32_t *) &buf[CHECK_OFFSET3]) = lba;
        off += NVMEH_BLK_SZ;
        lba = (lba == NVME_TEST_BUF_SIZE) ? 1 : lba + 1;
    }

    if (pthread_spin_init (&done_spin, 0)) {
        free (ctx);
        free (write_buffer);
        return -1;
    }

    return 0;
}

static void nvme_test_destroy (void)
{
    pthread_spin_destroy (&done_spin);
    free (write_buffer);
    free (ctx);
}

/* Runs the test */
void nvme_test_run (void)
{
    int it, ret;
    uint64_t slba, boff, lbaio, iosz;

    slba = firstlba;
    lbaio = NVME_TEST_LBA_IO;

    GET_NANOSECONDS(start,ts_s);
    for (it = 0; it < iocount; it++) {
        ctx[it].id = it;
        ctx[it].slba = slba;

        ctx[it].nlb = (slba + lbaio > lastlba) ? lastlba - slba + 1 : lbaio;
        
        boff = ((slba - 1) % NVME_TEST_LBA_BUF) * NVMEH_BLK_SZ;
        iosz = (slba + lbaio > lastlba) ?
                                ctx[it].nlb * NVMEH_BLK_SZ : NVME_TEST_IO_SZ;

        /* Submit the write command with callback function */
        ret = nvmeh_write (&write_buffer[boff],
                                    iosz, slba, nvme_test_callback, &ctx[it]);
        if (ret) {
            printf ("I/O %d not submitted.\n", it);
            pthread_spin_lock (&done_spin);
            done += ctx[it].nlb;
            pthread_spin_unlock (&done_spin);
        }

        /* Increase slba */
        slba += lbaio;
    }

    /* Wait until the all I/Os complete */
    while (done <= lastlba - firstlba) {
        usleep (500);
    }
}

int main (int argc, char **argv)
{
    int ret, q_id;

    /* Get arguments and setup first/last LBA */
    firstlba = 1;
    iocount  = NVME_TEST_IOS;
    if (argc > 1) {
        firstlba = atoi(argv[1]);
        if (!firstlba)
            firstlba = 1;
    }
    lastlba = firstlba + (NVME_TEST_IOS * NVME_TEST_LBA_IO) - 1;
    if (argc > 2) {
        lastlba = firstlba + atoi(argv[2]) - 1;
        iocount = ((uint64_t) atoi(argv[2]) * (uint64_t) NVMEH_BLK_SZ) /
                                                                NVME_TEST_IO_SZ;
        if (( atoi(argv[2]) * NVMEH_BLK_SZ) % NVME_TEST_IO_SZ != 0)
            iocount++;
    }

    /* Initialize OX NVMe Host */
    ret = nvmeh_init ();
    if (ret) {
        printf ("Failed to initializing NVMe Host.\n");
        return -1;
    }

    /* Add a network connections to be used by the Fabrics */
    nvme_host_add_server_iface (OXF_ADDR_1, OXF_PORT_1);

#if OXF_FULL_IFACES
    nvme_host_add_server_iface (OXF_ADDR_2, OXF_PORT_2);
    nvme_host_add_server_iface (OXF_ADDR_3, OXF_PORT_3);
    nvme_host_add_server_iface (OXF_ADDR_4, OXF_PORT_4);
#endif

    /* Create the NVMe queues. One additional queue for the admin queue */
    for (q_id = 0; q_id < NVME_TEST_NUM_QUEUES + 1; q_id++) {
        if (nvme_host_create_queue (q_id)) {
            printf ("Failed to creating queue %d.\n", q_id);
            goto EXIT;
        }
    }

    done = 0;
    err = 0;

    nvme_test_prepare ();
    nvme_test_run ();
    nvme_test_destroy ();
    nvme_test_print ();

    /* Closes the application */
EXIT:
    while (q_id) {
        q_id--;
        nvme_host_destroy_queue (q_id);
    }
    nvmeh_exit ();

    return 0;
}
