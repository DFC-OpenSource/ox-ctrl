/* OX: Open-Channel NVM Express SSD Controller
 * 
 *  - NVMe over Fabrics Command Parser
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
#include <stdint.h>
#include <libox.h>
#include <nvme.h>
#include <nvmef.h>

#define PARSER_FABRICS_COUNT   1

extern struct core_struct core;

static int parser_fabrics_connect (NvmeRequest *req, NvmeCmd *cmd)
{
    NvmefConnect *connect = (NvmefConnect *) cmd;

    if (nvmef_create_queue (connect->qid)) {
        req->status = 0x4000 | NVME_INTERNAL_DEV_ERROR;
        return -1;
    }

    return NVME_SUCCESS;
}

static int parser_fabrics_exec (NvmeRequest *req, NvmeCmd *cmd)
{
    NvmefCmd *fcmd = (NvmefCmd *) cmd;

    if (core.debug)
        printf ("[FABRICS: cftype: 0x%02x]\n", fcmd->fctype);

    switch (fcmd->fctype) {        
        case NVMEF_CMD_CONNECT:
            return parser_fabrics_connect (req, cmd);
            break;
        
        /* Commands not supported yet */
        case NVMEF_CMD_PROP_SET:
        case NVMEF_CMD_PROP_GET:
        case NVMEF_CMD_AUTH_SEND:
        case NVMEF_CMD_AUTH_RECV:
        default:
            log_err ("[fabrics: Command %x not supported.]", fcmd->fctype);
            req->status = NVME_INVALID_OPCODE;
            return -1;
    }
}

static struct nvm_parser_cmd fabrics_cmds[PARSER_FABRICS_COUNT] = {
    {
        .name       = "COMMAND_FABRICS",
        .opcode     = 0x7f,
        .opcode_fn  = parser_fabrics_exec,
        .queue_type = NVM_CMD_ADMIN
    }
};

static void parser_fabrics_exit (struct nvm_parser *parser)
{

}

static struct nvm_parser parser_fabrics = {
    .name       = "FABRICS_PARSER",
    .cmd_count  = PARSER_FABRICS_COUNT,
    .exit       = parser_fabrics_exit
};

int parser_fabrics_init (void)
{
    parser_fabrics.cmd = fabrics_cmds;

    return ox_register_parser (&parser_fabrics);
}