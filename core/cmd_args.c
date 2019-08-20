/* OX: Open-Channel NVM Express SSD Controller
 *
 *  - OX Controller argument parser
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

#include <argp.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <libox.h>

extern struct core_struct core;
const char *argp_program_version = OX_LABEL;
const char *argp_program_bug_address = "Ivan L. Picoli <ivpi@itu.dk>";

static char doc_global[] = "\n*** OX Controller " OX_VER " - " LABEL " ***\n"
        " \n The DFC Open-Channel SSD Controller\n\n"
        " Available commands:\n"
        "  start            Start controller with standard configuration\n"
        "  debug            Start controller and print Admin/IO commands\n"
        "  reset            Start controller and reset FTL\n"
        "                       WARNING (reset): ALL DATA WILL BE LOST\n"
        "  admin            Execute specific tasks within the controller\n"
        " \n Initial release developed by Ivan L. Picoli <ivpi@itu.dk>\n\n";

static char doc_admin[] =
        "\nUse this command to run specific tasks within the controller.\n"
        "\n Examples:"
        "\n  Show all available Admin tasks:"
        "\n    ox-ctrl admin -l\n"
        "\n  Run a specific admin task:"
        "\n    ox-ctrl admin -t <task_name>";

static struct argp_option opt_admin[] = {
    {"list", 'l', "list", OPTION_ARG_OPTIONAL,"Show available admin tasks."},
    {"task", 't', "admin_task", 0, "Admin task to be executed. <char>"},
    {0}
};

static error_t parse_opt_admin(int key, char *arg, struct argp_state *state)
{
    struct nvm_init_arg *args = state->input;

    switch (key) {
        case 't':
            if (!arg || strlen(arg) == 0 || strlen(arg) > CMDARG_LEN)
                argp_usage(state);
            strcpy(args->admin_task,arg);
            args->arg_num++;
            args->arg_flag |= CMDARG_FLAG_T;
            break;
        case 'l':
            args->arg_num++;
            args->arg_flag |= CMDARG_FLAG_L;
            break;
        case ARGP_KEY_END:
            if (args->arg_num > 1 || args->arg_num == 0)
                argp_usage(state);
            break;
        case ARGP_KEY_ARG:
        case ARGP_KEY_NO_ARGS:
        case ARGP_KEY_ERROR:
        case ARGP_KEY_SUCCESS:
        case ARGP_KEY_FINI:
        case ARGP_KEY_INIT:
            break;
        default:
            return ARGP_ERR_UNKNOWN;
    }

    return 0;
}

static void cmd_prepare(struct argp_state *state, struct nvm_init_arg *args,
                                              char *cmd, struct argp *argp_cmd)
{
    /* Remove the first arg from the parser */
    int argc = state->argc - state->next + 1;
    char** argv = &state->argv[state->next - 1];
    char* argv0 = argv[0];

    argv[0] = ox_malloc(strlen(state->name) + strlen(cmd) + 2, OX_MEM_CMD_ARG);
    if(!argv[0])
        argp_failure(state, 1, ENOMEM, 0);

    sprintf(argv[0], "%s %s", state->name, cmd);

    argp_parse(argp_cmd, argc, argv, ARGP_IN_ORDER, &argc, args);

    ox_free(argv[0], OX_MEM_CMD_ARG);
    argv[0] = argv0;
    state->next += argc - 1;
}

static struct argp argp_admin = {opt_admin, parse_opt_admin, 0, doc_admin};

error_t parse_opt (int key, char *arg, struct argp_state *state)
{
    struct nvm_init_arg *args = state->input;

    switch(key)
    {
        case ARGP_KEY_ARG:
            if (strcmp(arg, "start") == 0)
                args->cmdtype = CMDARG_START;
            else if (strcmp(arg, "debug") == 0)
                args->cmdtype = CMDARG_DEBUG;
            else if (strcmp(arg, "reset") == 0)
                args->cmdtype = CMDARG_RESET;
            else if (strcmp(arg, "admin") == 0) {
                args->cmdtype = CMDARG_ADMIN;
                cmd_prepare(state, args, "admin", &argp_admin);
            }
            break;
        default:
            return ARGP_ERR_UNKNOWN;
    }
    return 0;
}

void ox_cmdarg_exit (void)
{
    ox_free (core.args_global, OX_MEM_CMD_ARG);
}

static struct argp argp_global={NULL, parse_opt,"ox-ctrl [<cmd> [cmd-options]]",
                                                                   doc_global};

int ox_cmdarg_init (int argc, char **argv)
{
    if (!ox_mem_create_type ("CMD_ARG", OX_MEM_CMD_ARG))
        return -1;

    core.args_global = ox_malloc (sizeof (struct nvm_init_arg), OX_MEM_CMD_ARG);
    if (!core.args_global)
        return -1;

    memset (core.args_global, 0, sizeof(struct nvm_init_arg));

    argp_parse(&argp_global, argc, argv, ARGP_IN_ORDER, NULL, core.args_global);

    switch (core.args_global->cmdtype)
    {
        case CMDARG_START:
            return OX_RUN_MODE;
        case CMDARG_DEBUG:
            core.debug |= 1 << 0;
            return OX_RUN_MODE;
        case CMDARG_RESET:
            core.reset |= 1 << 0;
            return OX_RUN_MODE;
        case CMDARG_ADMIN:
            if (core.args_global->arg_flag & CMDARG_FLAG_L)
                core.nostart = 1;
            return OX_ADMIN_MODE;
        default:
            printf("Invalid command, please use --help to see more info.\n");
    }

    ox_free (core.args_global, OX_MEM_CMD_ARG);
    return -1;
}