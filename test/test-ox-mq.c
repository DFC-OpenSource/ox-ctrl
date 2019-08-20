#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <libox.h>
#include <ox-mq.h>

#define QUEUE_SIZE  512
#define N_QUEUES    4
#define N_CMD       (1024 * 1024 * N_QUEUES)
#define N_CORES     8

struct test_struct {
    uint64_t id;
    uint32_t queue;
};

static struct ox_mq *test_mq;
volatile static uint64_t completed [N_QUEUES];

static struct timespec ts, te;
static uint64_t start, end;

struct test_struct **cmd;

static void test_process_sq (struct ox_mq_entry *req)
{
RETRY:
    if (ox_mq_complete_req (test_mq, req))
        goto RETRY;
}

static void test_process_cq (void *opaque)
{
    struct test_struct *cmd = (struct test_struct *) opaque;
    uint32_t qid = cmd->queue;
    
    free (opaque);
    completed[qid]++;
}

static void test_process_to (void **opaque, int counter)
{
    
}

static void *test_submit_th (void *arg)
{
    uint64_t ent_i;
    uint64_t count = N_CMD / N_QUEUES;
    uint64_t qid = *(int *) arg;
    cpu_set_t cpuset;
    pthread_t current_thread;

    CPU_ZERO(&cpuset);
    CPU_SET(((qid % N_CORES) + N_QUEUES) % N_CORES, &cpuset);

    current_thread = pthread_self();
    pthread_setaffinity_np(current_thread, sizeof(cpu_set_t), &cpuset);

    printf ("Thread %lu set to core %lu\n", qid,
                                        ((qid % N_CORES) + N_QUEUES) % N_CORES);

    for (ent_i = 0; ent_i < count; ent_i++) {
        cmd[(count * qid) + ent_i]->queue = qid;
RETRY:
        if (ox_mq_submit_req (test_mq, qid, cmd[(count * qid) + ent_i]))
            goto RETRY;
    }

    return NULL;
}

int main (void)
{
    int ent_i, i;
    uint64_t total, qid;
    struct ox_mq_config config;
    pthread_t th[N_QUEUES];
    int th_qid[N_QUEUES];

    if (ox_mem_init ())
        return -1;

    if (!ox_mem_create_type ("OX_MQ", OX_MEM_OX_MQ))
        return -1;

    config.n_queues = N_QUEUES;
    config.q_size = QUEUE_SIZE;
    config.sq_fn = test_process_sq;
    config.cq_fn = test_process_cq;
    config.to_fn = test_process_to;
    config.to_usec = 0;
    config.flags = OX_MQ_CPU_AFFINITY;

    /* Set thread affinity*/
    for (qid = 0; qid < N_QUEUES; qid++) {
        printf ("Queue %lu - SQ set to core %lu, CQ set to core %lu\n", qid,
                (qid % N_CORES),
                ((qid % N_CORES) + N_QUEUES) % N_CORES);

        config.sq_affinity[qid] |= ((uint64_t) 1 << (qid % N_CORES));
        config.cq_affinity[qid] |=
                    ((uint64_t) 1 << (((qid % N_CORES) + N_QUEUES) % N_CORES));
    }

    test_mq = ox_mq_init(&config);
    if (!test_mq)
        return -1;

    for (ent_i = 0; ent_i < N_QUEUES; ent_i++)
        completed[ent_i] = 0;

    GET_NANOSECONDS(start, ts);

    cmd = malloc (sizeof (struct test_struct *) * N_CMD);
    if (!cmd)
        return -1;

    for (ent_i = 0; ent_i < N_CMD; ent_i++) {
        cmd[ent_i] = malloc (sizeof(struct test_struct));
    }
        
    for (ent_i = 0; ent_i < N_QUEUES; ent_i++) {
        th_qid[ent_i] = ent_i;
        if (pthread_create (&th[ent_i], NULL, test_submit_th, &th_qid[ent_i]))
            return -1;
    }

    total = 0;
    while (total < N_CMD) {
        usleep (10);
        total = 0;
        for (i = 0; i < N_QUEUES; i++) 
            total += completed[i];
    }
    GET_NANOSECONDS(end, te);

    printf ("\n");
    printf ("Time elapsed; %.3lf ms\n", (float)(end - start) / 1000 / 1000);
    printf ("IOPS: %lf\n", (N_CMD / ((float)(end - start) / 1000 / 1000 / 1000)));

    free (cmd);
    ox_mq_destroy (test_mq);
    ox_mem_exit ();

    return 0;
}