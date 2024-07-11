#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include <nvme-host.h>
#include <ox-fabrics.h>

#define NVMEH_NUM_QUEUES 4
#define NVMEH_BLKSZ 	 4096

#define OX_FUSE_WRITE_BACK 1

/* This is an example context that identifies the completion */
struct nvme_context {
    uint32_t id;
    uint64_t slba;
    uint64_t nlb;
    uint16_t done;
    uint16_t status;
    uint16_t is_write;
    uint8_t *buf;
    TAILQ_ENTRY (nvme_context) entry;
};

struct nvme_context *ctx;

TAILQ_HEAD (ctx_free, nvme_context)  ctx_f;
TAILQ_HEAD (ctx_used, nvme_context)  ctx_u;

static pthread_spinlock_t ctx_spin;

/* Callback funcion */
static void nvme_callback (void *c, uint16_t status)
{
    struct nvme_context *my_ctx = (struct nvme_context *) c;
    uint8_t freem = 0;

    if (OX_FUSE_WRITE_BACK && my_ctx->is_write)
	freem = 1;

    my_ctx->status = status;
    my_ctx->done = 1;

    if (freem) {
	free (my_ctx->buf);

	pthread_spin_lock (&ctx_spin);
	TAILQ_REMOVE(&ctx_u, my_ctx, entry);
	TAILQ_INSERT_TAIL(&ctx_f, my_ctx, entry);
	pthread_spin_unlock (&ctx_spin);
    }
}

int fuse_ox_read (uint64_t slba, uint64_t size, uint8_t *buf) {
    struct nvme_context *c;
    uint16_t retry = 2000;
    int ret;

RETRY:
    /* Get the context */
    pthread_spin_lock (&ctx_spin);
    c = TAILQ_FIRST(&ctx_f);
    if (!c) {
        pthread_spin_unlock (&ctx_spin);
        usleep (1000);
        retry--;
        if (retry) {
            goto RETRY;
	} else {
	    printf ("No slots available in the queue.\n");
            return -1;
	}
    }
    TAILQ_REMOVE(&ctx_f, c, entry);
    TAILQ_INSERT_TAIL(&ctx_u, c, entry);
    pthread_spin_unlock (&ctx_spin);
    
    /* Set the context */
    c->slba = slba;
    c->nlb = size / 4096;
    c->done = 0;
    c->status = 0;
    c->is_write = 0;
    
    /* Submit the read to OX */
    ret = nvmeh_read (buf, size, slba, nvme_callback, c);
    if (ret) {
	printf ("Error: I/O not submitted.\n");
	c->done = 1;
	c->status = 1;
    }

    /* Synchronously wait for completion */
    while (!c->done)
	usleep (1);

    ret = c->status;

   /* Free the context */ 
    pthread_spin_lock (&ctx_spin);
    TAILQ_REMOVE(&ctx_u, c, entry);
    TAILQ_INSERT_TAIL(&ctx_f, c, entry);
    pthread_spin_unlock (&ctx_spin);
    
    return ret;
}

int fuse_ox_write (uint64_t slba, uint64_t size, uint8_t *buf) {
    struct nvme_context *c;
    uint16_t retry = 2000;
    int ret, fail = 0;
    uint8_t *rbuf;

RETRY:
    /* Get the context */
    pthread_spin_lock (&ctx_spin);
    c = TAILQ_FIRST(&ctx_f);
    if (!c) {
        pthread_spin_unlock (&ctx_spin);
        usleep (1000);
        retry--;
        if (retry) {
            goto RETRY;
	} else {
	    printf ("No slots available in the queue.\n");
            return -1;
	}
    }
    TAILQ_REMOVE(&ctx_f, c, entry);
    TAILQ_INSERT_TAIL(&ctx_u, c, entry);
    pthread_spin_unlock (&ctx_spin);
    
    /* Set the context */
    c->slba = slba;
    c->nlb = size / 4096;
    c->done = 0;
    c->status = 0;
    c->is_write = 1;

    if (OX_FUSE_WRITE_BACK) {
	c->buf = malloc (size);
	if (!c->buf) {
	    fail = 1;
	    rbuf = buf;
	} else {
	    memcpy (c->buf, buf, size);
	    rbuf = c->buf;
	}
    } else {
	rbuf = buf;
    }
    
    /* Submit the read to OX */
    ret = nvmeh_write (rbuf, size, slba, nvme_callback, c);
    if (ret) {
	printf ("Error: I/O not submitted.\n");
	c->done = 1;
	c->status = 1;
    }

    if (OX_FUSE_WRITE_BACK && !fail) {
	ret = 0;
    } else {
	/* Synchronously wait for completion */
	while (!c->done)
	    usleep (1);

	ret = c->status;

	/* Free the context */
	pthread_spin_lock (&ctx_spin);
	TAILQ_REMOVE(&ctx_u, c, entry);
	TAILQ_INSERT_TAIL(&ctx_f, c, entry);
	pthread_spin_unlock (&ctx_spin);
    }

    return ret;
}

static int fuse_ox_prepare (void) {
    ctx = malloc (sizeof (struct nvme_context) * OXF_QUEUE_SIZE);
    if (!ctx)
	return -1;

    TAILQ_INIT(&ctx_f);
    TAILQ_INIT(&ctx_u);

    for (int ent_i = 0; ent_i < OXF_QUEUE_SIZE; ent_i++) {
	memset (&ctx[ent_i], 0, sizeof (struct nvme_context));
	ctx[ent_i].id = ent_i;
	TAILQ_INSERT_TAIL(&ctx_f, &ctx[ent_i], entry);
    }

    if (pthread_spin_init (&ctx_spin, 0))
	goto FREE;

    return 0;

FREE:
    free (ctx);
    return -1;
}

void fuse_ox_exit (void) {
    pthread_spin_destroy (&ctx_spin);
    free (ctx);
}

int fuse_ox_init (void)
{
    struct nvme_id_ctrl ctrl_id;
    struct nvme_id_ns ns_id;
    int ret, q_id = 0;

    ret = nvmeh_init ();
    if (ret) {
        printf ("Failed to initializing NVMe Host.\n");
        return -1;
    }

    if (fuse_ox_prepare ()) {
	goto ERROR;
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
            goto ERROR_Q;
        }
    }

    if (nvmeh_identify_ctrl (&ctrl_id)) {
	printf ("Identify Controller Failed!\n\n");
    } else {
	printf ("Identify Controller Success!\n");
	printf ("  Serial Number: %s\n", ctrl_id.sn);
	printf ("  Model Number : %s\n", ctrl_id.mn);
	printf ("  Ctrl Name    : %s\n\n", ctrl_id.subnqn);
    }

    if (nvmeh_identify_ns (&ns_id, 1)) {
	printf ("Identify Namespace Failed!\n\n");
    } else {
	printf ("Identify Namespace Success!\n");
	printf ("  Namespace ID   : 1\n");
	printf ("  Namespace Size : %lu blocks (%.2f GB)\n\n", ns_id.nsze,
		    (float) ns_id.nsze * NVMEH_BLK_SZ / 1024 / 1024 / 1024);
    }

    return 0;

    /* Closes the application */
ERROR_Q:
    fuse_ox_exit ();
ERROR:
    while (q_id) {
        q_id--;
        nvme_host_destroy_queue (q_id);
    }
    nvmeh_exit ();

    return -1;
}
