/* OX: Open-Channel NVM Express SSD Controller
 * 
 *  - LightNVM Command Parser
 *
 * Copyright 2016 IT University of Copenhagen
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
#include <stdint.h>
#include <libox.h>
#include <nvme.h>

#define PARSER_LNVM_COUNT   2

/*static void lnvm_debug_print_io (struct nvm_ppa_addr *list, uint64_t *prp,
               uint64_t *md_prp, uint16_t size, uint64_t dt_sz, uint64_t md_sz)
{
    int i;

    printf(" Number of sectors: %d\n", size);
    printf(" DMA size: %lu (data) + %lu (meta) = %lu bytes\n",
                                                 dt_sz, md_sz, dt_sz + md_sz);
    for (i = 0; i < size; i++)
        printf (" [ppa(%d): ch: %d, lun: %d, blk: %d, pl: %d, pg: %d, "
                                "sec: %d] prp: 0x%016lx\n", i, list[i].g.ch,
                                list[i].g.lun, list[i].g.blk, list[i].g.pl,
                                list[i].g.pg, list[i].g.sec, prp[i]);
    printf (" [meta_prp(0): 0x%016lx\n", md_prp[0]);

    fflush (stdout);
}*/

static int parser_lnvm_command1 (NvmeRequest *req, NvmeCmd *cmd)
{
    return 0;
}

static int parser_lnvm_command2 (NvmeRequest *req, NvmeCmd *cmd)
{
    return 0;
}

static struct nvm_parser_cmd lnvm_cmds[PARSER_LNVM_COUNT] = {
    {
        .name       = "COMMAND_1",
        .opcode     = 0xe2,
        .opcode_fn  = parser_lnvm_command1,
        .queue_type = NVM_CMD_ADMIN
    },
    {
        .name       = "COMMAND_2",
        .opcode     = 0x91,
        .opcode_fn  = parser_lnvm_command2,
        .queue_type = NVM_CMD_IO
    }
};

static void parser_lnvm_exit (struct nvm_parser *parser)
{

}

static struct nvm_parser parser_lnvm = {
    .name       = "LNVM_PARSER",
    .cmd_count  = PARSER_LNVM_COUNT,
    .exit       = parser_lnvm_exit
};

int parser_lnvm_init (void)
{
    parser_lnvm.cmd = lnvm_cmds;

    return ox_register_parser (&parser_lnvm);
}