/* OX: Open-Channel NVM Express SSD Controller
 *
 *  - OX-App Flash Translation Layer (Global Provisioning)
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

#include <stdlib.h>
#include <stdio.h>
#include <ox-app.h>
#include <pthread.h>
#include <time.h>
#include <stdint.h>
#include <string.h>
#include <libox.h>

extern uint16_t app_nch;
static struct app_channel **ch;
static pthread_spinlock_t cur_ch_spin;
static u_atomic_t cur_ch_id;

static int gl_prov_init (void)
{
    uint32_t nch;
    struct app_rec_entry *cp_entry;

    if (!ox_mem_create_type ("OXBLK_GL_PROV", OX_MEM_OXBLK_GPR))
        return -1;

    ch = ox_malloc (sizeof (struct app_channel *) * app_nch, OX_MEM_OXBLK_GPR);
    if (!ch)
        return -1;

    cur_ch_id.counter = U_ATOMIC_INIT_RUNTIME(0);
    if (pthread_spin_init (&cur_ch_spin, 0))
        goto FREE;

    cp_entry = oxapp()->recovery->get_fn (APP_CP_GL_PROV_CH);
    if (cp_entry)
        u_atomic_set (&cur_ch_id, *((uint32_t *) cp_entry->data));

    nch = oxapp()->channels.get_list_fn (ch, app_nch);
    if (nch != app_nch)
        goto SPIN_LOCK;

    log_info("    [ox-blk: Global Provisioning started.]\n");

    return 0;

SPIN_LOCK:
    pthread_spin_destroy (&cur_ch_spin);
FREE:
    ox_free (ch, OX_MEM_OXBLK_GPR);
    return -1;
}

static void gl_prov_exit (void)
{
    pthread_spin_destroy (&cur_ch_spin);
    ox_free (ch, OX_MEM_OXBLK_GPR);

    log_info("    [ox-blk: Global Provisioning stopped.]\n");
}

static struct app_prov_ppas *gl_prov_get_ppa_list (uint32_t pgs, uint8_t type)
{
    uint32_t ch_id, act_ch_id, nact_ch, cc, new_cc, nppas, tppas, pg_left, i;
    struct app_prov_ppas      tmp_ppa[app_nch];
    struct app_channel       *dec_ch[app_nch];
    struct nvm_ppa_addr      *list;
    struct nvm_mmgr_geometry *g;
    uint16_t                  pgs_ch[app_nch];

    struct app_prov_ppas *prov_ppa = ox_malloc (sizeof (struct app_prov_ppas),
                                                              OX_MEM_OXBLK_GPR);
    if (!prov_ppa)
        return NULL;

    prov_ppa->ch = ox_malloc (sizeof (struct app_channel *) * app_nch,
                                                              OX_MEM_OXBLK_GPR);
    if (!prov_ppa->ch)
        goto FREE_PPA;

    prov_ppa->nch = app_nch;
    tppas = 0;

    nact_ch = 0;
    for (ch_id = 0; ch_id < app_nch; ch_id++) {
        tmp_ppa[ch_id].nppas = 0;
        tmp_ppa[ch_id].nch = 0;
        tmp_ppa[ch_id].ppa = NULL;

        /* collect active channels add channel current users*/
        if (app_ch_active(ch[ch_id])) {

            app_ch_inc_thread(ch[ch_id]);
            if (!app_ch_active(ch[ch_id])) {
                app_ch_dec_thread(ch[ch_id]);
                prov_ppa->ch[ch_id] = dec_ch[ch_id] = NULL;
                continue;
            }

            prov_ppa->ch[ch_id] = dec_ch[ch_id] = ch[ch_id];
            nact_ch++;
            continue;
        }

        prov_ppa->ch[ch_id] = NULL;
        dec_ch[ch_id] = NULL;
    }

    if (APP_DEBUG_GL_PROV)
        printf ("\n[ox-blk (gl_prov): Active Channels: %d]\n", nact_ch);

    if (!nact_ch)
        return NULL;

REDIST:
    /* Collect the current ch and set the new current ch for the next thread */
    pthread_spin_lock (&cur_ch_spin);
    cc = u_atomic_read (&cur_ch_id);
    new_cc = (pgs % app_nch) + cc;
    if (new_cc > app_nch - 1)
        new_cc -= app_nch;
    u_atomic_set (&cur_ch_id, new_cc);
    pthread_spin_unlock (&cur_ch_spin);

    /* Distribute the pages among the active channels */
    pg_left = pgs;
    act_ch_id = 0;
    memset (&pgs_ch, 0x0, sizeof(uint16_t) * app_nch);
    while(pg_left) {
        pg_left--;
        pgs_ch[act_ch_id]++;
        act_ch_id = (act_ch_id == nact_ch - 1) ? 0 : act_ch_id + 1;
    }

    pg_left = pgs;
    ch_id = cc;
    act_ch_id = 0;
    while (pg_left) {
        /* NULL pointers are inactive channels */
        if (prov_ppa->ch[ch_id]) {

            g = ch[ch_id]->ch->geometry;
            nppas = g->sec_per_pg * g->n_of_planes * pgs_ch[act_ch_id];

            /* Get all pages per channel at once */
            list = ox_calloc (sizeof (struct nvm_ppa_addr) * nppas, 1,
                                                            OX_MEM_OXBLK_GPR);

            if (oxapp()->ch_prov->get_ppas_fn (
                                    ch[ch_id], list, pgs_ch[act_ch_id], type)) {
                /* Mark the channel as inactive and redistribute the remaining
                 * pages */
                app_ch_dec_thread(ch[ch_id]);
                app_ch_need_gc_set(ch[ch_id]);
                app_ch_active_unset(ch[ch_id]);

                prov_ppa->ch[ch_id] = dec_ch[ch_id] = NULL;
                nact_ch--;
                pgs = pg_left;
                ox_free (list, OX_MEM_OXBLK_GPR);

                if (nact_ch > 0)
                    goto REDIST;
                else
                    goto FREE_CH;
            }

            tppas += nppas;
            tmp_ppa[ch_id].nppas += nppas;
            tmp_ppa[ch_id].ppa = ox_realloc (tmp_ppa[ch_id].ppa,
                        sizeof (struct nvm_ppa_addr) * tmp_ppa[ch_id].nppas,
                        OX_MEM_OXBLK_GPR);

            memcpy (tmp_ppa[ch_id].ppa + tmp_ppa[ch_id].nppas - nppas, list,
                                        sizeof (struct nvm_ppa_addr) * nppas);

            ox_free (list, OX_MEM_OXBLK_GPR);
            dec_ch[ch_id] = NULL;

            pg_left -= pgs_ch[act_ch_id];
            act_ch_id = (act_ch_id == nact_ch - 1) ? 0 : act_ch_id + 1;
        }
        ch_id = (ch_id == app_nch - 1) ? 0 : ch_id + 1;
    }

    prov_ppa->ppa = ox_calloc (sizeof (struct nvm_ppa_addr) * tppas, 1,
                                                              OX_MEM_OXBLK_GPR);
    if (!prov_ppa->ppa)
        goto DEC_CH;

    /* Reorder PPA list for maximum parallelism */
    nppas = tppas;
    ch_id = cc;
    while (nppas) {
        if (tmp_ppa[ch_id].nppas > 0) {
            g = ch[ch_id]->ch->geometry;

            memcpy (&prov_ppa->ppa[tppas - nppas],
                    &tmp_ppa[ch_id].ppa[tmp_ppa[ch_id].nch],
                    sizeof (struct nvm_ppa_addr) * g->sec_per_pg *
                    g->n_of_planes);

            tmp_ppa[ch_id].nppas -= g->sec_per_pg * g->n_of_planes;
            tmp_ppa[ch_id].nch += g->sec_per_pg * g->n_of_planes;
            nppas -= g->sec_per_pg * g->n_of_planes;
        }
        ch_id = (ch_id == app_nch - 1) ? 0 : ch_id + 1;
    }
    prov_ppa->nppas = tppas;

    if (APP_DEBUG_GL_PROV)
        printf ("\n[ox-blk (gl_prov): GET - %d ppas]\n", tppas);

    for (ch_id = 0; ch_id < app_nch; ch_id++) {
        if (dec_ch[ch_id] != NULL)
            app_ch_dec_thread(prov_ppa->ch[ch_id]);

        if (tmp_ppa[ch_id].ppa != NULL)
            ox_free (tmp_ppa[ch_id].ppa, OX_MEM_OXBLK_GPR);

        if (APP_DEBUG_GL_PROV)
            printf (" [ox-blk (gl_prov): GET - Ch %d, %d ppas, %d users]\n",
                      ch_id, tmp_ppa[ch_id].nch, app_ch_nthreads(ch[ch_id]));
    }

    return prov_ppa;

DEC_CH:
    for (i = 0; i < prov_ppa->nch; i++) {
        if (prov_ppa->ch[i] != NULL)
            app_ch_dec_thread(prov_ppa->ch[i]);
    }
FREE_CH:
    ox_free (prov_ppa->ch, OX_MEM_OXBLK_GPR);
FREE_PPA:
    ox_free (prov_ppa, OX_MEM_OXBLK_GPR);
    return NULL;
}

static void gl_prov_free_ppa_list (struct app_prov_ppas *ppas)
{
    uint32_t i;

    if (!ppas) {
        log_err ("[ox-blk (gl_prov): NULL pointer. Ch users is unstable.]");
        return;
    }

    if (APP_DEBUG_GL_PROV)
        printf ("\n[appnvm (gl_prov): FREE - %d ppas]\n", ppas->nppas);

    for (i = 0; i < ppas->nch; i++) {
        if (ppas->ch[i] != NULL)
            app_ch_dec_thread(ppas->ch[i]);
        if (APP_DEBUG_GL_PROV)
            printf (" [appnvm (gl_prov): FREE Ch %d - %d users]\n", i,
                                                    app_ch_nthreads(ch[i]));
    }

    ox_free (ppas->ch, OX_MEM_OXBLK_GPR);
    ox_free (ppas->ppa, OX_MEM_OXBLK_GPR);
    ox_free (ppas, OX_MEM_OXBLK_GPR);
}

static struct app_gl_prov oxblk_gl_prov = {
    .mod_id           = OXBLK_GL_PROV,
    .name             = "OX-BLOCK-GPROV",
    .init_fn          = gl_prov_init,
    .exit_fn          = gl_prov_exit,
    .new_fn           = gl_prov_get_ppa_list,
    .free_fn          = gl_prov_free_ppa_list
};

void oxb_gl_prov_register (void) {
    app_mod_register (APPMOD_GL_PROV, OXBLK_GL_PROV, &oxblk_gl_prov);
}

uint32_t get_gl_prov_current_ch (void)
{
    return (uint32_t) u_atomic_read (&cur_ch_id);
}