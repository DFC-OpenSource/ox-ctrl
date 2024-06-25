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
#define NVME_TEST_IO_SZ         (1024 * 256) /* 256 KB */
#define NVME_TEST_LBA_IO        (NVME_TEST_IO_SZ / NVMEH_BLK_SZ)

/* Number of I/Os */
#define NVME_TEST_IOS           4000

/* Offsets containing the LBA for read check */
#define CHECK_OFFSET1           16
#define CHECK_OFFSET2           1852
#define CHECK_OFFSET3           3740
#define CHECK_OFFSET_LBA1	458
#define CHECK_OFFSET_LBA2	2008
#define CHECK_OFFSET_LBA3	3478

/* Used to keep track of completed LBAs */
static volatile uint64_t   done, corr;
static pthread_spinlock_t  done_spin;

/* Time and statistics variables */
static struct timespec ts_s, ts_e, ts_c;
static uint64_t start, end, cur, firstlba, lastlba, iocount, err;

/* This is an example context that identifies the completion */
struct nvme_test_context {
    uint32_t    id;
    uint64_t    slba;
    uint64_t    nlb;
    uint8_t     *buf;
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
    printf ("\r %d %% - Time %.2lf ms, read: %.2lf MB, thput: %.2lf MB/s, "
            "IOPS: %.1f", perc, sec * divt, mb, mb / sec, iops);
}

/* Callback funcion */
static void nvme_test_callback (void *ctx, uint16_t status)
{
    struct nvme_test_context *my_ctx = (struct nvme_test_context *) ctx;
    uint32_t *check1, *check2, *check3, lba, check;
    uint64_t *checklba1, *checklba2, *checklba3, checklba, slba;
    uint8_t *buf, fail;

    if (status)
        printf ("I/O %d failed.\n", my_ctx->id);

    pthread_spin_lock (&done_spin);
    done += my_ctx->nlb;
    if (status)
        err++;
    pthread_spin_unlock (&done_spin);

    if (done % (NVME_TEST_LBA_IO * 50) == 0)
        nvme_test_print_runtime ();

    slba = my_ctx->slba;
    for (lba = 0; lba < my_ctx->nlb; lba++) {
        buf = &my_ctx->buf[NVMEH_BLK_SZ * lba];

	/* Check the relative LBA offsets */
	check1 = (uint32_t *) &buf[CHECK_OFFSET1];
        check2 = (uint32_t *) &buf[CHECK_OFFSET2];
        check3 = (uint32_t *) &buf[CHECK_OFFSET3];
        check = ((uint32_t) slba + lba) % NVME_TEST_LBA_BUF;
        if (!check) check = NVME_TEST_LBA_BUF;

	/* Check the absolute lba offsets */
	checklba1 = (uint64_t *) &buf[CHECK_OFFSET_LBA1];
	checklba2 = (uint64_t *) &buf[CHECK_OFFSET_LBA2];
	checklba3 = (uint64_t *) &buf[CHECK_OFFSET_LBA3];
	checklba = slba + (uint64_t) lba;

	fail = 0;
        if (*check1 != check || *check2 != check || *check3 != check ||
	    *checklba1 != checklba || *checklba2 != checklba ||
	    *checklba3 != checklba) {
		corr++;
		fail++;
	}

	if (fail)
	    printf ("\nDATA CORRUPTION -> LBA: %lu, read LBA: %lu/%lu/%lu\n"
		      "                   check: %d, read %d/%d/%d\n",
		      checklba, *checklba1, *checklba2, *checklba3,
		      check, *check1, *check2, *check3);
    }
    free (my_ctx->buf);
          
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
    printf (    " Read LBAs     : %lu (%lu to %lu) \n",
                                    lastlba - firstlba + 1, firstlba, lastlba);
    printf (    " Read data     : %.2lf MB\n", mb);
    printf (    " Throughput    : %.2lf MB/s\n", mb / sec);
    printf (    " IOPS          : %.1f\n", iops);
    printf (    " Block size    : %d KB\n", NVMEH_BLK_SZ / 1024);
    printf (    " I/O size      : %d KB\n", NVME_TEST_IO_SZ / 1024);
    printf (    " Issued I/Os   : %lu\n", iocount);
    printf (    " Failed I/Os   : %lu\n\n", err);
    if (corr)
        printf (" Corrupted LBAs: %lu\n", corr);
}

/* Prepares the write buffer and spinlocks */
static int nvme_test_prepare (void)
{
    ctx = malloc (iocount * sizeof (struct nvme_test_context));
    if (!ctx) {
        printf ("Memory allocation error.\n");
        return -1;
    }

    if (pthread_spin_init (&done_spin, 0)) {
        free (ctx);
        return -1;
    }

    return 0;
}

static void nvme_test_destroy (void)
{
    pthread_spin_destroy (&done_spin);
    free (ctx);
}

/* Runs the test */
void nvme_test_run (void)
{
    int it, ret;
    uint64_t slba, lbaio, iosz;

    slba = firstlba;
    lbaio = NVME_TEST_LBA_IO;

    GET_NANOSECONDS(start,ts_s);
    for (it = 0; it < iocount; it++) {
        ctx[it].id = it;
        ctx[it].slba = slba;

        ctx[it].nlb = (slba + lbaio > lastlba) ? lastlba - slba + 1 : lbaio;

        ctx[it].buf = malloc (NVME_TEST_IO_SZ);
        if (!ctx[it].buf) {
            ret = -1;
            goto CHECK;
        }

        iosz = (slba + lbaio > lastlba) ?
                                ctx[it].nlb * NVMEH_BLK_SZ : NVME_TEST_IO_SZ;

        /* Submit the read command with callback function */
        ret = nvmeh_read (ctx[it].buf,
                                    iosz, slba, nvme_test_callback, &ctx[it]);
CHECK:
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
    for (q_id = 0; q_id < NVME_TEST_NUM_QUEUES; q_id++) {
        if (nvme_host_create_queue (q_id)) {
            printf ("Failed to creating queue %d.\n", q_id);
            goto EXIT;
        }
    }

    corr = 0;
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

