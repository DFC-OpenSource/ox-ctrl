/* OX: Open-Channel NVM Express SSD Controller
 *
 *  - OX Tests Core
 *
 * Copyright (C) 2016, IT University of Copenhagen. All rights reserved.
 * Written by Ivan Luiz Picoli <ivpi@itu.dk>
 *
 * Funding support provided by CAPES Foundation, Ministry of Education
 * of Brazil, Brasilia - DF 70040-020, Brazil.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  - Redistributions of source code must retain the above copyright notice,
 *  this list of conditions and the following disclaimer.
 *  - Redistributions in binary form must reproduce the above copyright notice,
 *  this list of conditions and the following disclaimer in the documentation
 *  and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <argp.h>
#include <sys/queue.h>
#include <string.h>
#include <time.h>
#include "../include/ssd.h"
#include "../include/tests.h"
#include "../include/nvme.h"

static struct tests_ctrl tests_u;
extern struct core_struct core;

atomic_t         pgs_ok;
pthread_mutex_t  pgs_ok_mutex;
volatile uint64_t t_usec;
pthread_mutex_t  usec_mutex;
int              rand_lun[4];
int              rand_blk[4];

uint64_t tests_get_cmd_usec (struct nvm_mmgr_io_cmd *cmd)
{
    return (cmd->tend.tv_sec*(uint64_t)1000000+cmd->tend.tv_usec) -
                    (cmd->tstart.tv_sec*(uint64_t)1000000+cmd->tstart.tv_usec);
}

static struct tests_set *tests_find_set (char *name)
{
    struct tests_set *set;
    LIST_FOREACH(set, &tests_u.sets_head, entry){
        if(strcmp(set->name,name) == 0)
            return set;
    }
    return NULL;
}

static struct tests_test *tests_find_test (struct tests_set *set, char *name)
{
    struct tests_test *test;
    LIST_FOREACH(test, &set->tests_head, entry) {
        if(strcmp(test->name,name) == 0)
            return test;
    }

    return NULL;
}

int tests_register_set (struct tests_set *set)
{
    if (!set || !set->name || !set->desc)
        goto ERR;

    if (strlen(set->name) > CMDARG_LEN || strlen(set->name) < 1)
        goto ERR;

    if (tests_find_set(set->name))
        goto ERR;

    LIST_INSERT_HEAD(&tests_u.sets_head, set, entry);
    tests_u.ns++;
    set->nt = 0;

    return 0;
ERR:
    printf("Error (tests_register_set). Aborting tests.\n");
    return -1;
}

int tests_register (struct tests_test *test, struct tests_set *set)
{
    if (!set || !tests_find_set (set->name))
        goto ERR;

    if (!test || !test->run_fn || !test->name || !test->desc)
        goto ERR;

    if (strlen(test->name) > CMDARG_LEN || strlen(test->name) < 1)
        goto ERR;

    if (tests_find_test(set, test->name))
        goto ERR;

    LIST_INSERT_HEAD(&set->tests_head, test, entry);
    set->nt++;

    return 0;
ERR:
    printf("Error (tests_register). Aborting tests.\n");
    return -1;
}

static int tests_register_all (struct nvm_init_arg *args)
{
    /* All Media Managers test set */
    if(testset_mmgr_dfcnand_init (args))
        return -1;

    /* LightNVM test set */
    if(testset_lnvm_init (args))
        return -1;

    return 0;
}

static void tests_show_all ()
{
    struct tests_set *set;
    struct tests_test *test;

    printf("\n The test unit contains %d set(s):\n", tests_u.ns);
    LIST_FOREACH(set, &tests_u.sets_head, entry){
        printf("\n  SET: '%s' - %d test(s)\n",set->name,set->nt);
        printf("  description: %s\n",set->desc);
        LIST_FOREACH(test, &set->tests_head, entry) {
            printf("\n    - '%s': %s\n",test->name, test->desc);
        }
    }
    printf("\n");
}

int tests_init (struct nvm_init_arg *args)
{
    struct tests_set *set;
    struct tests_test *test;

    tests_u.sets_head.lh_first = NULL;
    tests_u.ns = 0;
    tests_u.n_run = 0;
    tests_u.n_success = 0;

    srand(time (NULL));
    if(tests_register_all(args))
        return -1;

    /* List all tests */
    if (args->arg_flag & CMDARG_FLAG_L) {
        tests_show_all ();
        return -1;
    }

    /* Add all tests to the run queue */
    if (args->arg_flag & CMDARG_FLAG_A) {
        LIST_FOREACH(set, &tests_u.sets_head, entry){
            LIST_FOREACH(test, &set->tests_head, entry) {
                test->flags |= TEST_FLAG_QUEUED;
            }

        }
        return 0;
    }

    /* Add a single test to the run queue */
    if ((args->arg_flag & CMDARG_FLAG_S) && (args->arg_flag & CMDARG_FLAG_T)) {
        set = tests_find_set(args->test_setname);
        if (!set) {
            printf("SET not found.\n");
            return -1;
        }

        test = tests_find_test(set, args->test_subtest);
        if (!test) {
            printf("TEST not found.\n");
            return -1;
        }

        test->flags |= TEST_FLAG_QUEUED;

        return 0;
    }

    /* Add a whole set to the run queue */
    if (args->arg_flag & CMDARG_FLAG_S) {
        set = tests_find_set(args->test_setname);
        if (!set) {
            printf("SET not found.\n");
            return -1;
        }

        LIST_FOREACH(test, &set->tests_head, entry) {
            test->flags |= TEST_FLAG_QUEUED;
        }
        return 0;
    }

    return -1;
}

static void tests_show_statistics () {
    struct tests_set *set;
    struct tests_test *test;

    printf("\n RESULTS:\n");
    printf("\n  TOTAL TESTS: %d",tests_u.n_run);
    printf("\n  SUCCESS:     %d", tests_u.n_success);
    printf("\n  FAILED:      %d\n", tests_u.n_run - tests_u.n_success);

    if (tests_u.n_run - tests_u.n_success == 0)
        goto OUT;

    printf("\n  FAILED TESTS:");
    LIST_FOREACH(set, &tests_u.sets_head, entry){
        LIST_FOREACH(test, &set->tests_head, entry) {
            if ((test->flags & TEST_FLAG_QUEUED) &&
                                            !(test->flags & TEST_FLAG_SUCCESS))
                printf("\n    - SET: %s, TEST: %s", set->name, test->name);
        }
    }

OUT:
    printf("\n");
}

void *tests_start (void *arg)
{
    struct tests_set *set;
    struct tests_test *test;

    t_usec = 0;

    printf(" \n TESTS STARTED...\n");
    LIST_FOREACH(set, &tests_u.sets_head, entry){
        if (!LIST_EMPTY(&set->tests_head))
            printf("\n  SET: %s\n",set->name);

        LIST_FOREACH(test, &set->tests_head, entry) {
            if (test->flags & TEST_FLAG_QUEUED) {
                tests_u.n_run++;

                printf("    - %s...\n",test->name);
                test->flags |= !(test->run_fn(test)) ? TEST_SUCCESS : TEST_FAIL;
                if (test->flags & TEST_FLAG_SUCCESS) {
                    tests_u.n_success++;
                    printf("    OK\n");
                } else {
                    printf("    FAIL\n");
                }
            }
        }
    }

    tests_show_statistics();
}

void *tests_admin (void *arg)
{
    ox_admin_init (core.args_global);
}

void tests_complete_io  (NvmeRequest *req)
{
    int i;
    for (i = 0; i < req->nvm_io.status.total_pgs; i++) {
        pthread_mutex_lock(&usec_mutex);
        t_usec += tests_get_cmd_usec(&req->nvm_io.mmgr_io[i]);
        pthread_mutex_unlock(&usec_mutex);

        pthread_mutex_lock(&pgs_ok_mutex);
        atomic_inc(&pgs_ok);
        pthread_mutex_unlock(&pgs_ok_mutex);
    }
}

struct tests_init_st tests_is = {
    .init           = tests_init,
    .start          = tests_start,
    .admin          = tests_admin,
    .complete_io    = tests_complete_io
};