#ifndef TESTS_H
#define TESTS_H

#include "ssd.h"
#include "nvme.h"
#include "lightnvm.h"

#define TEST_FLAG_SUCCESS   0x1
#define TEST_FLAG_QUEUED    0x2

#define TEST_SUCCESS        TEST_FLAG_SUCCESS
#define TEST_FAIL           0x0

struct tests_test;

typedef int (tests_run_fn)(struct tests_test *);

struct tests_test {
    char                    *name;
    char                    *desc;
    tests_run_fn            *run_fn;
    uint8_t                 flags;
    LIST_ENTRY(tests_test)  entry;
};

struct tests_set {
    char                                *name;
    char                                *desc;
    uint16_t                            nt;
    LIST_HEAD(test_list, tests_test)    tests_head;
    LIST_ENTRY(tests_set)               entry;
};

struct tests_ctrl {
    LIST_HEAD(set_list, tests_set)  sets_head;
    uint16_t                        ns;
    uint16_t                        n_run;
    uint16_t                        n_success;
};

struct tests_io_request {
    struct NvmeRequest  *req;
    struct NvmeCmd      *cmd;
    uint16_t            n_sec;
};

int tests_register (struct tests_test *, struct tests_set *);
int tests_register_set (struct tests_set *);

int testset_mmgr_dfcnand_init (struct nvm_init_arg *);
int testset_lnvm_init (struct nvm_init_arg *);
int ox_admin_init (struct nvm_init_arg *);

uint64_t tests_get_cmd_usec (struct nvm_mmgr_io_cmd *);

#endif /* TESTS_H */