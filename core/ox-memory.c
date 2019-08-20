/* OX: Open-Channel NVM Express SSD Controller
 *
 *  - OX Memory Control
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

#ifndef __USE_GNU
#define __USE_GNU
#endif

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <sys/queue.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <search.h>
#include <libox.h>

#define OX_MEM_MAX_TYPES    64

#define OX_MEM_MALLOC   0
#define OX_MEM_CALLOC   1

struct ox_mem_node {
    void        *ptr;
    uint64_t     size;
};

struct ox_mem_type {
    char                name[32];
    uint16_t            id;
    volatile uint64_t   allocd;
    pthread_spinlock_t  alloc_spin;
    TAILQ_ENTRY(ox_mem_type)  entry;

    /* RB tree */
    void *root;
};

struct ox_memory {
    struct ox_mem_type  *types[OX_MEM_MAX_TYPES];
    TAILQ_HEAD(types_list, ox_mem_type) types_head;
};

static struct ox_memory ox_mem;

static void ox_mem_btree_free_act (void *nodep)
{
    struct ox_mem_node *datap = (struct ox_mem_node *) nodep;

    log_info ("[mem: Leak detected. ptr: %p, size %lu bytes]",
                                                       datap->ptr, datap->size);
    free (datap->ptr);
    free (datap);
}

#if OX_MEM_MANAGER
static int ox_mem_btree_compare(const void *new_ptr, const void *tree_ptr)
{
    struct ox_mem_node *tree_node = (struct ox_mem_node *) tree_ptr;
    struct ox_mem_node *new_node = (struct ox_mem_node *) new_ptr;

    if ( new_node->ptr < tree_node->ptr )
        return -1;
    if ( new_node->ptr > tree_node->ptr )
        return 1;
    return 0;
}

static void *ox_alloc (size_t size, uint16_t type, uint8_t fn)
{
    void *ptr;
    struct ox_mem_node *node;

    if (type >= OX_MEM_MAX_TYPES || !ox_mem.types[type])
        return NULL;

    ptr = (fn == OX_MEM_CALLOC) ? calloc (1, size) : malloc (size);
    if (!ptr)
        return NULL;

    node = malloc (sizeof (struct ox_mem_node));
    if (!node) {
        free (ptr);
        return NULL;
    }

    node->ptr = ptr;
    node->size = size;

    pthread_spin_lock (&ox_mem.types[type]->alloc_spin);
    ox_mem.types[type]->allocd += size;
    tsearch((void *) node, &ox_mem.types[type]->root, ox_mem_btree_compare);
    pthread_spin_unlock (&ox_mem.types[type]->alloc_spin);

    return ptr;
}

void *ox_calloc (size_t members, size_t size, uint16_t type)
{
    return ox_alloc (members * size, type, OX_MEM_CALLOC);
}

void *ox_malloc (size_t size, uint16_t type)
{
    return ox_alloc (size, type, OX_MEM_MALLOC);
}

void *ox_realloc (void *ptr, size_t size, uint16_t type)
{
    struct ox_mem_node **ret;
    struct ox_mem_node *node;
    void *new;

    if (type >= OX_MEM_MAX_TYPES || !ox_mem.types[type])
        return NULL;

    if (!ptr)
        return ox_alloc (size, type, OX_MEM_MALLOC);

    pthread_spin_lock (&ox_mem.types[type]->alloc_spin);

    ret = (struct ox_mem_node **) tfind(&ptr, &ox_mem.types[type]->root,
                                                        ox_mem_btree_compare);
    if (!ret) {
        log_err ("[mem: Realloc pointer not found. ptr: %p, type %d]", ptr, type);
        pthread_spin_unlock (&ox_mem.types[type]->alloc_spin);
        return NULL;
    }

    new = realloc (ptr, size);
    if (!new)
        return NULL;

    node = *ret;
    tdelete(node, &ox_mem.types[type]->root, ox_mem_btree_compare);
    node->ptr = new;
    tsearch((void *) node, &ox_mem.types[type]->root, ox_mem_btree_compare);

    ox_mem.types[type]->allocd += size - node->size;
    node->size = size;

    pthread_spin_unlock (&ox_mem.types[type]->alloc_spin);

    return new;
}

void *ox_free (void *ptr, uint16_t type)
{
    struct ox_mem_node **ret;
    struct ox_mem_node *node;

    if (type >= OX_MEM_MAX_TYPES || !ox_mem.types[type])
        return ptr;

    pthread_spin_lock (&ox_mem.types[type]->alloc_spin);

    ret = (struct ox_mem_node **) tfind(&ptr, &ox_mem.types[type]->root,
                                                        ox_mem_btree_compare);
    if (!ret) {
        log_err ("[mem: Double free detected. ptr: %p, type %d]", ptr, type);
        pthread_spin_unlock (&ox_mem.types[type]->alloc_spin);
        return ptr;
    }

    node = *ret;
    ox_mem.types[type]->allocd -= node->size;

    free (node->ptr);    

    tdelete(node, &ox_mem.types[type]->root, ox_mem_btree_compare);

    free (node);

    pthread_spin_unlock (&ox_mem.types[type]->alloc_spin);

    return NULL;
}
#endif

struct ox_mem_type *ox_mem_create_type (const char *name, uint16_t type)
{
    struct ox_mem_type *memtype;

    if (strlen(name) > 32) {
        log_err ("[mem: Type name is too big. max of 32 bytes]");
        return NULL;
    }

    if (type >= OX_MEM_MAX_TYPES) {
        log_err ("[mem: Invalid Type ID: %d]", type);
        return NULL;
    }

    if (ox_mem.types[type]) {
        log_err ("[mem: Memory Type already created: %d]", type);
        return NULL;
    }
    
    memtype = malloc (sizeof (struct ox_mem_type));
    if (!memtype)
        return NULL;

    memtype->id = type;
    memcpy (memtype->name, name, strlen(name) + 1);

    memtype->allocd = 0;
    memtype->root = NULL;

    if (pthread_spin_init (&memtype->alloc_spin, 0)) {
        free (memtype);
        return NULL;
    }

    TAILQ_INSERT_TAIL (&ox_mem.types_head, memtype, entry);
    ox_mem.types[type] = memtype;

    return memtype;
}

static void ox_mem_destroy_type (struct ox_mem_type *type)
{
    if (!type || !ox_mem.types[type->id])
        return;

    if (type->allocd) {
        log_info ("[mem: Leak was found in %s]", type->name);
        //printf ("\n[mem: Leak was found in %s. Check the log.]\n", type->name);
        tdestroy (ox_mem.types[type->id]->root,ox_mem_btree_free_act);
        type->allocd = 0;
    }

    TAILQ_REMOVE (&ox_mem.types_head, type, entry);
    ox_mem.types[type->id] = NULL;
    pthread_spin_destroy (&type->alloc_spin);
    free (type);
}

uint64_t ox_mem_size (uint16_t type)
{
    if (type >= OX_MEM_MAX_TYPES)
        return 0;

    return ox_mem.types[type]->allocd;
}

uint64_t ox_mem_total (void)
{
    struct ox_mem_type *memtype;
    uint64_t total = 0;

    TAILQ_FOREACH (memtype, &ox_mem.types_head, entry) {
        total += ox_mem_size (memtype->id);
    }

    return total;
}

void ox_mem_print_memory (void)
{
    struct ox_mem_type *memtype;
    uint64_t total = 0;

    TAILQ_FOREACH (memtype, &ox_mem.types_head, entry) {
        printf (" %2d - %-18s: %10.5lf MB (%lu bytes)\n", memtype->id, memtype->name,
                (double) memtype->allocd / (double) 1024 / (double) 1024,
                memtype->allocd);
        total += ox_mem_size (memtype->id);
    }
    printf ("\n Total: %.5lf MB (%lu bytes)\n",
                        (double) total / (double) 1024 / (double) 1024, total);
}

void ox_mem_exit (void)
{
    struct ox_mem_type *memtype;

    while (!TAILQ_EMPTY(&ox_mem.types_head)) {
        memtype = TAILQ_FIRST(&ox_mem.types_head);
        ox_mem_destroy_type (memtype);
    }

    log_info ("[ox: Memory Management stopped.]");
}

int ox_mem_init (void)
{
    if (!TAILQ_EMPTY(&ox_mem.types_head))
        return 0;

    TAILQ_INIT (&ox_mem.types_head);
    memset (ox_mem.types, 0x0, OX_MEM_MAX_TYPES * sizeof(struct ox_mem_type *));

    log_info ("[ox: Memory Management started.]");

    return 0;
}